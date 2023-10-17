// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "tt_metal/impl/dispatch/command_queue.hpp"

#include "debug_tools.hpp"
#include "device_data.hpp"
#include "noc/noc_parameters.h"
#include "tt_metal/detail/program.hpp"
#include "tt_metal/detail/tt_metal.hpp"
#include "tt_metal/host_api.hpp"
#include "tt_metal/impl/buffers/semaphore.hpp"
#include "tt_metal/third_party/umd/device/tt_xy_pair.h"
// XXXX TODO(PGK): fix include paths so device can export interfaces
#include "tt_metal/src/firmware/riscv/common/dev_msgs.h"
#include <algorithm> // for copy() and assign()
#include <iterator> // for back_inserter


static constexpr u32 HUGE_PAGE_SIZE = 1024 * 1024 * 1024;

namespace tt::tt_metal {

#include "tt_metal/third_party/tracy/public/tracy/Tracy.hpp"

u32 get_noc_multicast_encoding(const CoreCoord& top_left, const CoreCoord& bottom_right) {
    return NOC_MULTICAST_ENCODING(top_left.x, top_left.y, bottom_right.x, bottom_right.y);
}

u32 align(u32 addr, u32 alignment) { return ((addr - 1) | (alignment - 1)) + 1; }

u32 noc_coord_to_u32(CoreCoord coord) { return NOC_XY_ENCODING(NOC_X(coord.x), NOC_Y(coord.y)); }

ProgramMap ConstructProgramMap(const Device* device, Program& program) {
    /*
        TODO(agrebenisan): Move this logic to compile program
    */
    vector<transfer_info> program_page_transfers;
    vector<transfer_info> host_page_transfers;
    vector<u32> num_transfers_in_program_pages; // This is a vector that corresponds to the number of transfers within program pages acros all program pages
    vector<u32> num_transfers_in_host_data_pages; // Same thing, but for runtime arg pages
    u32 num_transfers_within_page = 0;

    u32 src = 0;
    constexpr static u32 noc_transfer_alignment_in_bytes = 16;
    auto update_program_page_transfers = [&num_transfers_within_page](
                                             u32 src,
                                             u32 num_bytes,
                                             u32 dst,
                                             vector<transfer_info>& transfers,
                                             vector<u32>& num_transfers_per_page,
                                             const vector<pair<u32, u32>>& dst_noc_multicast_info) -> u32 {
        while (num_bytes) {
            u32 num_bytes_left_in_page = DeviceCommand::PROGRAM_PAGE_SIZE - (src % DeviceCommand::PROGRAM_PAGE_SIZE);
            u32 num_bytes_in_transfer = std::min(num_bytes_left_in_page, num_bytes);
            src = align(src + num_bytes_in_transfer, noc_transfer_alignment_in_bytes);

            u32 multicast_instruction_idx = 1;
            for (const auto& [dst_noc_multicast_encoding, num_receivers] : dst_noc_multicast_info) {
                bool last = multicast_instruction_idx == dst_noc_multicast_info.size();
                transfer_info transfer_instruction = {.size_in_bytes = num_bytes_in_transfer, .dst = dst, .dst_noc_multicast_encoding = dst_noc_multicast_encoding, .num_receivers = num_receivers, .last_multicast_in_group = last};
                transfers.push_back(transfer_instruction);
                num_transfers_within_page++;
                multicast_instruction_idx++;
            }

            dst += num_bytes_in_transfer;
            num_bytes -= num_bytes_in_transfer;

            if ((src % DeviceCommand::PROGRAM_PAGE_SIZE) == 0) {
                num_transfers_per_page.push_back(num_transfers_within_page);
                num_transfers_within_page = 0;
            }
        }

        return src;
    };

    auto extract_dst_noc_multicast_info = [&device](const set<CoreRange>& ranges) -> vector<pair<u32, u32>> {
        // This API extracts all the pairs of noc multicast encodings given a set of core ranges
        vector<pair<u32, u32>> dst_noc_multicast_info;
        for (const CoreRange& core_range : ranges) {
            CoreCoord physical_start = device->worker_core_from_logical_core(core_range.start);
            CoreCoord physical_end = device->worker_core_from_logical_core(core_range.end);

            u32 dst_noc_multicast_encoding = get_noc_multicast_encoding(physical_start, physical_end);

            u32 num_receivers = core_range.size();
            dst_noc_multicast_info.push_back(std::make_pair(dst_noc_multicast_encoding, num_receivers));
        }
        return dst_noc_multicast_info;
    };

    static const map<RISCV, u32> processor_to_l1_arg_base_addr = {
        {RISCV::BRISC, BRISC_L1_ARG_BASE},
        {RISCV::NCRISC, NCRISC_L1_ARG_BASE},
        {RISCV::COMPUTE, TRISC_L1_ARG_BASE},
    };
    // Step 1: Get transfer info for runtime args (soon to just be host data). We
    // want to send host data first because of the higher latency to pull
    // in host data.
    for (const auto kernel_id : program.kernel_ids()) {
        const Kernel* kernel = detail::GetKernel(program, kernel_id);
        u32 dst = processor_to_l1_arg_base_addr.at(kernel->processor());
        for (const auto& [core_coord, runtime_args] : kernel->runtime_args()) {
            CoreCoord physical_core = device->worker_core_from_logical_core(core_coord);
            u32 num_bytes = runtime_args.size() * sizeof(u32);
            u32 dst_noc = get_noc_multicast_encoding(physical_core, physical_core);

            // Only one receiver per set of runtime arguments
            src = update_program_page_transfers(
                src, num_bytes, dst, host_page_transfers, num_transfers_in_host_data_pages, {{dst_noc, 1}});
        }
    }

    // Step 2: Continue constructing pages for circular buffer configs
    for (const shared_ptr<CircularBuffer>& cb : program.circular_buffers()) {
        vector<pair<u32, u32>> dst_noc_multicast_info = extract_dst_noc_multicast_info(cb->core_ranges().ranges());
        constexpr static u32 num_bytes = UINT32_WORDS_PER_CIRCULAR_BUFFER_CONFIG * sizeof(u32);
        for (const auto buffer_index : cb->buffer_indices()) {
            src = update_program_page_transfers(
                src,
                num_bytes,
                CIRCULAR_BUFFER_CONFIG_BASE + buffer_index * UINT32_WORDS_PER_CIRCULAR_BUFFER_CONFIG * sizeof(u32),
                host_page_transfers,
                num_transfers_in_host_data_pages,
                dst_noc_multicast_info);
        }
    }

    // Cleanup step of separating runtime arg pages from program pages
    if (num_transfers_within_page) {
        num_transfers_in_host_data_pages.push_back(num_transfers_within_page);
        num_transfers_within_page = 0;
    }

    static const map<RISCV, u32> processor_to_local_mem_addr = {
        {RISCV::BRISC, MEM_BRISC_INIT_LOCAL_L1_BASE},
        {RISCV::NCRISC, MEM_NCRISC_INIT_LOCAL_L1_BASE},
        {RISCV::TRISC0, MEM_TRISC0_INIT_LOCAL_L1_BASE},
        {RISCV::TRISC1, MEM_TRISC1_INIT_LOCAL_L1_BASE},
        {RISCV::TRISC2, MEM_TRISC2_INIT_LOCAL_L1_BASE}};

    // Step 3: Determine the transfer information for each program binary
    src = 0; // Restart src since it begins in a new page
    for (KernelID kernel_id : program.kernel_ids()) {
        const Kernel* kernel = detail::GetKernel(program, kernel_id);
        vector<pair<u32, u32>> dst_noc_multicast_info =
            extract_dst_noc_multicast_info(kernel->core_range_set().ranges());

        vector<RISCV> sub_kernels;
        if (kernel->processor() == RISCV::COMPUTE) {
            sub_kernels = {RISCV::TRISC0, RISCV::TRISC1, RISCV::TRISC2};
        } else {
            sub_kernels = {kernel->processor()};
        }

        u32 sub_kernel_index = 0;
        for (const ll_api::memory& kernel_bin : kernel->binaries()) {
            kernel_bin.process_spans([&](vector<u32>::const_iterator mem_ptr, u64 dst, u32 len) {
                u32 num_bytes = len * sizeof(u32);
                if ((dst & MEM_LOCAL_BASE) == MEM_LOCAL_BASE) {
                    dst = (dst & ~MEM_LOCAL_BASE) + processor_to_local_mem_addr.at(sub_kernels[sub_kernel_index]);
                } else if ((dst & MEM_NCRISC_IRAM_BASE) == MEM_NCRISC_IRAM_BASE) {
                    dst = (dst & ~MEM_NCRISC_IRAM_BASE) + MEM_NCRISC_INIT_IRAM_L1_BASE;
                }

                src = update_program_page_transfers(
                    src, num_bytes, dst, program_page_transfers, num_transfers_in_program_pages, dst_noc_multicast_info);
            });
            sub_kernel_index++;
        }
    }

    // Step 4: Continue constructing pages for semaphore configs
    for (const Semaphore& semaphore : program.semaphores()) {
        vector<pair<u32, u32>> dst_noc_multicast_info =
            extract_dst_noc_multicast_info(semaphore.core_range_set().ranges());

        src = update_program_page_transfers(
            src,
            SEMAPHORE_ALIGNMENT,
            semaphore.address(),
            program_page_transfers,
            num_transfers_in_program_pages,
            dst_noc_multicast_info);
    }

    // Step 5: Continue constructing pages for GO signals
    for (KernelGroup& kg : program.get_kernel_groups()) {
        kg.launch_msg.mode = DISPATCH_MODE_DEV;
        vector<pair<u32, u32>> dst_noc_multicast_info =
            extract_dst_noc_multicast_info(kg.core_ranges.ranges());

        src = update_program_page_transfers(
            src,
            sizeof(launch_msg_t),
            GET_MAILBOX_ADDRESS_HOST(launch),
            program_page_transfers,
            num_transfers_in_program_pages,
            dst_noc_multicast_info
        );
    }

    if (num_transfers_within_page) {
        num_transfers_in_program_pages.push_back(num_transfers_within_page);
    }

    // Create a vector of all program binaries/cbs/semaphores
    vector<u32> program_pages(align(src, DeviceCommand::PROGRAM_PAGE_SIZE) / sizeof(u32), 0);
    u32 program_page_idx = 0;
    for (KernelID kernel_id : program.kernel_ids()) {
        const Kernel* kernel = detail::GetKernel(program, kernel_id);

        for (const ll_api::memory& kernel_bin : kernel->binaries()) {
            kernel_bin.process_spans([&](vector<u32>::const_iterator mem_ptr, u64 dst, u32 len) {
                std::copy(mem_ptr, mem_ptr + len, program_pages.begin() + program_page_idx);
                program_page_idx = align(program_page_idx + len, noc_transfer_alignment_in_bytes / sizeof(u32));
            });
        }
    }

    for (const Semaphore& semaphore : program.semaphores()) {
        program_pages[program_page_idx] = semaphore.initial_value();
        program_page_idx += 4;
    }

    for (const KernelGroup& kg: program.get_kernel_groups()) {
        uint32_t *launch_message_data = (uint32_t *)&kg.launch_msg;
        program_pages[program_page_idx] = launch_message_data[0];
        program_pages[program_page_idx + 1] = launch_message_data[1];
        program_pages[program_page_idx + 2] = launch_message_data[2];
        program_pages[program_page_idx + 3] = launch_message_data[3];
        program_page_idx += 4;
    }

    return {
        .num_workers = u32(program.logical_cores().size()),
        .program_pages = std::move(program_pages),
        .program_page_transfers = std::move(program_page_transfers),
        .host_page_transfers = std::move(host_page_transfers),
        .num_transfers_in_program_pages = std::move(num_transfers_in_program_pages),
        .num_transfers_in_host_data_pages = std::move(num_transfers_in_host_data_pages),
    };
}

// EnqueueReadBufferCommandSection
EnqueueReadBufferCommand::EnqueueReadBufferCommand(
    Device* device, Buffer& buffer, vector<u32>& dst, SystemMemoryWriter& writer) :
    dst(dst), writer(writer), buffer(buffer) {
    this->device = device;
}

const DeviceCommand EnqueueReadBufferCommand::assemble_device_command(u32 dst_address) {
    DeviceCommand command;

    u32 padded_page_size = align(this->buffer.page_size(), 32);
    u32 data_size_in_bytes = padded_page_size * this->buffer.num_pages();

    command.add_buffer_transfer_instruction(
        this->buffer.address(),
        dst_address,
        this->buffer.num_pages(),
        padded_page_size,
        (u32)this->buffer.buffer_type(),
        u32(BufferType::SYSTEM_MEMORY));

    u32 consumer_cb_num_pages = (DeviceCommand::CONSUMER_DATA_BUFFER_SIZE / padded_page_size);

    if (consumer_cb_num_pages >= 4) {
        consumer_cb_num_pages = (consumer_cb_num_pages / 4) * 4;
        command.set_producer_consumer_transfer_num_pages(consumer_cb_num_pages / 4);
    } else {
        command.set_producer_consumer_transfer_num_pages(1);
    }

    u32 consumer_cb_size = consumer_cb_num_pages * padded_page_size;
    u32 producer_cb_num_pages = consumer_cb_num_pages * 2;
    u32 producer_cb_size = producer_cb_num_pages * padded_page_size;

    command.set_stall();
    command.set_page_size(padded_page_size);
    command.set_producer_cb_size(producer_cb_size);
    command.set_consumer_cb_size(consumer_cb_size);
    command.set_producer_cb_num_pages(producer_cb_num_pages);
    command.set_consumer_cb_num_pages(consumer_cb_num_pages);
    command.set_num_pages(this->buffer.num_pages());
    command.set_data_size(padded_page_size * this->buffer.num_pages());

    TT_ASSERT(padded_page_size <= consumer_cb_size, "Page is too large to fit in consumer buffer");
    return command;
}

void EnqueueReadBufferCommand::process() {
    u32 write_ptr = this->writer.cq_write_interface.fifo_wr_ptr << 4;
    u32 system_memory_temporary_storage_address = write_ptr + DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND;
    this->read_buffer_addr = system_memory_temporary_storage_address;
    const auto cmd = this->assemble_device_command(system_memory_temporary_storage_address);
    const auto command_desc = cmd.get_desc();
    vector<u32> command_vector(command_desc.begin(), command_desc.end());

    u32 num_pages = this->buffer.size() / this->buffer.page_size();
    u32 padded_page_size = align(this->buffer.page_size(), 32);
    u32 data_size_in_bytes = cmd.get_data_size();
    u32 cmd_size = DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND + data_size_in_bytes;

    this->writer.cq_reserve_back(this->device, cmd_size);
    this->writer.cq_write(this->device, command_vector, write_ptr);
    this->writer.cq_push_back(this->device, cmd_size);
}

EnqueueCommandType EnqueueReadBufferCommand::type() { return this->type_; }

// EnqueueWriteBufferCommand section
EnqueueWriteBufferCommand::EnqueueWriteBufferCommand(
    Device* device, Buffer& buffer, vector<u32>& src, SystemMemoryWriter& writer) :
    writer(writer), src(src), buffer(buffer) {
    TT_ASSERT(
        buffer.buffer_type() == BufferType::DRAM or buffer.buffer_type() == BufferType::L1,
        "Trying to write to an invalid buffer");

    this->device = device;
}

const DeviceCommand EnqueueWriteBufferCommand::assemble_device_command(u32 src_address) {
    DeviceCommand command;

    u32 padded_page_size = this->buffer.page_size();
    if (this->buffer.page_size() != this->buffer.size()) {
        padded_page_size = align(this->buffer.page_size(), 32);
    }
    u32 data_size_in_bytes = padded_page_size * this->buffer.num_pages();
    command.add_buffer_transfer_instruction(
        src_address,
        this->buffer.address(),
        this->buffer.num_pages(),
        padded_page_size,
        (u32) BufferType::SYSTEM_MEMORY,
        (u32) this->buffer.buffer_type());

    u32 consumer_cb_num_pages = (DeviceCommand::CONSUMER_DATA_BUFFER_SIZE / padded_page_size);

    if (consumer_cb_num_pages >= 4) {
        consumer_cb_num_pages = (consumer_cb_num_pages / 4) * 4;
        command.set_producer_consumer_transfer_num_pages(consumer_cb_num_pages / 4);
    } else {
        command.set_producer_consumer_transfer_num_pages(1);
    }

    u32 consumer_cb_size = consumer_cb_num_pages * padded_page_size;
    u32 producer_cb_num_pages = consumer_cb_num_pages * 2;
    u32 producer_cb_size = producer_cb_num_pages * padded_page_size;

    command.set_page_size(padded_page_size);
    command.set_producer_cb_size(producer_cb_size);
    command.set_consumer_cb_size(consumer_cb_size);
    command.set_producer_cb_num_pages(producer_cb_num_pages);
    command.set_consumer_cb_num_pages(consumer_cb_num_pages);
    command.set_num_pages(this->buffer.num_pages());
    command.set_data_size(padded_page_size * this->buffer.num_pages());

    TT_ASSERT(padded_page_size <= consumer_cb_size, "Page is too large to fit in consumer buffer");

    return command;
}

void EnqueueWriteBufferCommand::process() {
    u32 write_ptr = this->writer.cq_write_interface.fifo_wr_ptr << 4;
    u32 system_memory_temporary_storage_address = write_ptr + DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND;

    const auto cmd = this->assemble_device_command(system_memory_temporary_storage_address);
    const auto command_desc = cmd.get_desc();
    vector<u32> command_vector(command_desc.begin(), command_desc.end());
    u32 data_size_in_bytes = cmd.get_data_size();

    u32 cmd_size = DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND + data_size_in_bytes;
    this->writer.cq_reserve_back(this->device, cmd_size);
    this->writer.cq_write(this->device, command_vector, write_ptr);

    // Need to deal with the edge case where our page
    // size is not 32B aligned
    if (this->buffer.page_size() % 32 != 0 and this->buffer.page_size() != this->buffer.size()) {
        vector<u32>::const_iterator src_iterator = this->src.begin();
        u32 num_u32s_in_page = this->buffer.page_size() / sizeof(u32);
        u32 num_pages = this->buffer.num_pages();
        u32 dst = system_memory_temporary_storage_address;
        for (u32 i = 0; i < num_pages; i++) {
            vector<u32> src_page(src_iterator, src_iterator + num_u32s_in_page);
            this->writer.cq_write(this->device, src_page, dst);
            src_iterator += num_u32s_in_page;
            dst = align(dst + this->buffer.page_size(), 32);
        }
    } else {
        this->writer.cq_write(this->device, this->src, system_memory_temporary_storage_address);
    }

    this->writer.cq_push_back(this->device, cmd_size);
}

EnqueueCommandType EnqueueWriteBufferCommand::type() { return this->type_; }

EnqueueProgramCommand::EnqueueProgramCommand(
    Device* device,
    Buffer& buffer,
    ProgramMap& program_to_dev_map,
    SystemMemoryWriter& writer,
    vector<u32>& host_data,
    bool stall
    ) :
    buffer(buffer), program_to_dev_map(program_to_dev_map), writer(writer), host_data(host_data), stall(stall) {
    this->device = device;
}

const DeviceCommand EnqueueProgramCommand::assemble_device_command(u32 host_data_src) {
    DeviceCommand command;
    command.set_num_workers(this->program_to_dev_map.num_workers);

    auto populate_program_data_transfer_instructions =
        [&command](const vector<u32>& num_transfers_per_page, const vector<transfer_info>& transfers_in_pages) {
            u32 i = 0;
            for (u32 j = 0; j < num_transfers_per_page.size(); j++) {
                u32 num_transfers_in_page = num_transfers_per_page[j];
                command.write_program_entry(num_transfers_in_page);
                for (u32 k = 0; k < num_transfers_in_page; k++) {
                    const auto [num_bytes, dst, dst_noc, num_receivers, last_multicast_in_group] = transfers_in_pages[i];
                    command.add_write_page_partial_instruction(num_bytes, dst, dst_noc, num_receivers, last_multicast_in_group);
                    i++;
                }
            }
        };

    command.set_is_program();

    // Not used, since we specified that this is a program command, and the consumer just looks at the write program
    // info
    constexpr static u32 dummy_dst_addr = 0;
    constexpr static u32 dummy_buffer_type = 0;
    u32 num_host_data_pages = this->program_to_dev_map.num_transfers_in_host_data_pages.size();
    u32 num_program_binary_pages = this->program_to_dev_map.num_transfers_in_program_pages.size();
    u32 num_pages = num_host_data_pages + num_program_binary_pages;
    command.set_page_size(DeviceCommand::PROGRAM_PAGE_SIZE);
    command.set_num_pages(num_pages);
    command.set_data_size(
        DeviceCommand::PROGRAM_PAGE_SIZE *
        num_host_data_pages);  // Only the runtime args are part of the device command

    if (num_host_data_pages != 0) {
        command.add_buffer_transfer_instruction(
            host_data_src,
            dummy_dst_addr,
            num_host_data_pages,
            DeviceCommand::PROGRAM_PAGE_SIZE,
            u32(BufferType::SYSTEM_MEMORY),
            dummy_buffer_type);
        populate_program_data_transfer_instructions(
            this->program_to_dev_map.num_transfers_in_host_data_pages, this->program_to_dev_map.host_page_transfers);
    }

    if (num_program_binary_pages != 0) {
        command.add_buffer_transfer_instruction(
            this->buffer.address(),
            dummy_dst_addr,
            num_program_binary_pages,
            DeviceCommand::PROGRAM_PAGE_SIZE,
            u32(this->buffer.buffer_type()),
            dummy_buffer_type);
        populate_program_data_transfer_instructions(
            this->program_to_dev_map.num_transfers_in_program_pages, this->program_to_dev_map.program_page_transfers);
    }

    constexpr static u32 producer_cb_num_pages = (DeviceCommand::PRODUCER_DATA_BUFFER_SIZE / DeviceCommand::PROGRAM_PAGE_SIZE);
    constexpr static u32 producer_cb_size = producer_cb_num_pages * DeviceCommand::PROGRAM_PAGE_SIZE;

    constexpr static u32 consumer_cb_num_pages = (DeviceCommand::CONSUMER_DATA_BUFFER_SIZE / DeviceCommand::PROGRAM_PAGE_SIZE);
    constexpr static u32 consumer_cb_size = consumer_cb_num_pages * DeviceCommand::PROGRAM_PAGE_SIZE;

    command.set_producer_cb_size(producer_cb_size);
    command.set_consumer_cb_size(consumer_cb_size);
    command.set_producer_cb_num_pages(producer_cb_num_pages);
    command.set_consumer_cb_num_pages(consumer_cb_num_pages);

    // Should only ever be set if we are
    // enqueueing a program immediately
    // after writing it to a buffer
    if (this->stall) {
        command.set_stall();
    }

    // This needs to be quite small, since programs are small
    command.set_producer_consumer_transfer_num_pages(4);

    return command;
}

void EnqueueProgramCommand::process() {
    u32 write_ptr = this->writer.cq_write_interface.fifo_wr_ptr << 4;
    u32 system_memory_temporary_storage_address = write_ptr + DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND;

    const DeviceCommand cmd = this->assemble_device_command(system_memory_temporary_storage_address);
    const auto command_desc = cmd.get_desc();
    vector<u32> command_vector(command_desc.begin(), command_desc.end());

    u32 data_size_in_bytes = cmd.get_data_size();
    const u32 cmd_size = DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND + data_size_in_bytes;
    this->writer.cq_reserve_back(this->device, cmd_size);
    this->writer.cq_write(this->device, command_vector, write_ptr);
    if (this->host_data.size() != 0) {
        this->writer.cq_write(this->device, this->host_data, system_memory_temporary_storage_address);
    }
    this->writer.cq_push_back(this->device, cmd_size);
}

EnqueueCommandType EnqueueProgramCommand::type() { return this->type_; }

// FinishCommand section
FinishCommand::FinishCommand(Device* device, SystemMemoryWriter& writer) : writer(writer) { this->device = device; }

const DeviceCommand FinishCommand::assemble_device_command(u32) {
    DeviceCommand command;
    command.finish();
    return command;
}

void FinishCommand::process() {
    u32 write_ptr = this->writer.cq_write_interface.fifo_wr_ptr << 4;
    const auto command_desc = this->assemble_device_command(0).get_desc();
    vector<u32> command_vector(command_desc.begin(), command_desc.end());

    u32 cmd_size = DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND;

    this->writer.cq_reserve_back(this->device, cmd_size);
    this->writer.cq_write(this->device, command_vector, write_ptr);
    this->writer.cq_push_back(this->device, cmd_size);
}

EnqueueCommandType FinishCommand::type() { return this->type_; }

// EnqueueWrapCommand section
EnqueueWrapCommand::EnqueueWrapCommand(Device* device, SystemMemoryWriter& writer) : writer(writer) {
    this->device = device;
}

const DeviceCommand EnqueueWrapCommand::assemble_device_command(u32) {
    DeviceCommand command;
    return command;
}

void EnqueueWrapCommand::process() {
    u32 write_ptr = this->writer.cq_write_interface.fifo_wr_ptr << 4;
    u32 space_left = HUGE_PAGE_SIZE - write_ptr;

    // Since all of the values will be 0, this will be equivalent to
    // a bunch of NOPs
    vector<u32> command_vector(space_left / sizeof(u32), 0);
    command_vector[0] = 1;  // wrap

    this->writer.cq_reserve_back(this->device, space_left);
    this->writer.cq_write(this->device, command_vector, write_ptr);
    this->writer.cq_push_back(this->device, space_left);
}

EnqueueCommandType EnqueueWrapCommand::type() { return this->type_; }

// Sending dispatch kernel. TODO(agrebenisan): Needs a refactor
void send_dispatch_kernel_to_device(Device* device) {
    ZoneScoped;
    // Ideally, this should be some separate API easily accessible in
    // TT-metal, don't like the fact that I'm writing this from scratch

    Program dispatch_program = Program();
    auto dispatch_cores = device->dispatch_cores().begin();
    CoreCoord producer_logical_core = *dispatch_cores++;
    CoreCoord consumer_logical_core = *dispatch_cores;

    CoreCoord producer_physical_core = device->worker_core_from_logical_core(producer_logical_core);
    CoreCoord consumer_physical_core = device->worker_core_from_logical_core(consumer_logical_core);

    std::map<string, string> producer_defines = {
        {"IS_DISPATCH_KERNEL", ""},
        {"CONSUMER_NOC_X", std::to_string(consumer_physical_core.x)},
        {"CONSUMER_NOC_Y", std::to_string(consumer_physical_core.y)},
    };
    std::map<string, string> consumer_defines = {
        {"PRODUCER_NOC_X", std::to_string(producer_physical_core.x)},
        {"PRODUCER_NOC_Y", std::to_string(producer_physical_core.y)},
    };
    std::vector<uint32_t> dispatch_compile_args = {DEVICE_DATA.TENSIX_SOFT_RESET_ADDR};
    tt::tt_metal::CreateDataMovementKernel(
        dispatch_program,
        "tt_metal/impl/dispatch/kernels/command_queue_producer.cpp",
        producer_logical_core,
        tt::tt_metal::DataMovementConfig {
            .processor = tt::tt_metal::DataMovementProcessor::RISCV_0,
            .noc = tt::tt_metal::NOC::RISCV_0_default,
            .compile_args = dispatch_compile_args,
            .defines = producer_defines});

    tt::tt_metal::CreateDataMovementKernel(
        dispatch_program,
        "tt_metal/impl/dispatch/kernels/command_queue_consumer.cpp",
        consumer_logical_core,
        tt::tt_metal::DataMovementConfig {
            .processor = tt::tt_metal::DataMovementProcessor::RISCV_0,
            .noc = tt::tt_metal::NOC::RISCV_0_default,
            .compile_args = dispatch_compile_args,
            .defines = consumer_defines});

    tt::tt_metal::CreateSemaphore(dispatch_program, {producer_logical_core, producer_logical_core}, 2);
    tt::tt_metal::CreateSemaphore(dispatch_program, {consumer_logical_core, consumer_logical_core}, 0);

    detail::CompileProgram(device, dispatch_program);
    tt::tt_metal::detail::ConfigureDeviceWithProgram(device, dispatch_program);

    u32 fifo_addr = (HOST_CQ_FINISH_PTR + 32) >> 4;
    vector<u32> fifo_addr_vector = {fifo_addr};
    tt::tt_metal::detail::WriteToDeviceL1(device, producer_logical_core, CQ_READ_PTR, fifo_addr_vector);
    tt::tt_metal::detail::WriteToDeviceL1(device, producer_logical_core, CQ_WRITE_PTR, fifo_addr_vector);

    // Initialize wr toggle
    vector<u32> toggle_start_vector = {0};
    tt::tt_metal::detail::WriteToDeviceL1(device, producer_logical_core, CQ_READ_TOGGLE, toggle_start_vector);
    tt::tt_metal::detail::WriteToDeviceL1(device, producer_logical_core, CQ_WRITE_TOGGLE, toggle_start_vector);

    launch_msg_t msg = dispatch_program.kernels_on_core(producer_logical_core)->launch_msg;

    // TODO(pkeller): Should use LaunchProgram once we have a mechanism to avoid running all RISCs
    tt::llrt::write_launch_msg_to_core(device->id(), producer_physical_core, &msg);
    tt::llrt::write_launch_msg_to_core(device->id(), consumer_physical_core, &msg);
}

// CommandQueue section
CommandQueue::CommandQueue(Device* device) {
    vector<u32> pointers(CQ_START / sizeof(u32), 0);
    pointers[0] = CQ_START >> 4;  // rd ptr (96 >> 4 = 6)

    tt::Cluster::instance().write_sysmem_vec(pointers, 0, 0);

    send_dispatch_kernel_to_device(device);
    this->device = device;
}

CommandQueue::~CommandQueue() {}

void CommandQueue::enqueue_command(shared_ptr<Command> command, bool blocking) {
    // For the time-being, doing the actual work of enqueing in
    // the main thread.
    // TODO(agrebenisan): Perform the following in a worker thread
    command->process();

    if (blocking) {
        this->finish();
    }
}

void CommandQueue::enqueue_read_buffer(Buffer& buffer, vector<u32>& dst, bool blocking) {
    ZoneScopedN("CommandQueue_read_buffer");
    u32 read_buffer_command_size = DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND + buffer.size();
    if ((this->sysmem_writer.cq_write_interface.fifo_wr_ptr << 4) + read_buffer_command_size >= HUGE_PAGE_SIZE) {
        tt::log_assert(read_buffer_command_size <= HUGE_PAGE_SIZE - 96, "EnqueueReadBuffer command is too large");
        this->wrap();
    }
    tt::log_debug(tt::LogDispatch, "EnqueueReadBuffer");

    shared_ptr<EnqueueReadBufferCommand> command =
        std::make_shared<EnqueueReadBufferCommand>(this->device, buffer, dst, this->sysmem_writer);

    // TODO(agrebenisan): Provide support so that we can achieve non-blocking
    // For now, make read buffer blocking since after the
    // device moves data into the buffer we want to read out
    // of, we then need to consume it into a vector. This
    // is easiest way to bring this up
    TT_ASSERT(blocking, "EnqueueReadBuffer only has support for blocking mode currently");
    this->enqueue_command(command, blocking);

    u32 num_pages = buffer.size() / buffer.page_size();
    u32 padded_page_size = align(buffer.page_size(), 32);
    u32 data_size_in_bytes = padded_page_size * num_pages;

    tt::Cluster::instance().read_sysmem_vec(dst, command->read_buffer_addr, data_size_in_bytes, 0);

    // This vector is potentially padded due to alignment constraints, so need to now remove the padding
    if ((buffer.page_size() % 32) != 0) {
        vector<u32> new_dst(buffer.size() / sizeof(u32), 0);
        u32 padded_page_size_in_u32s = align(buffer.page_size(), 32) / sizeof(u32);
        u32 new_dst_counter = 0;
        for (u32 i = 0; i < dst.size(); i += padded_page_size_in_u32s) {
            for (u32 j = 0; j < buffer.page_size() / sizeof(u32); j++) {
                new_dst[new_dst_counter] = dst[i + j];
                new_dst_counter++;
            }
        }
        dst = new_dst;
    }
}

void CommandQueue::enqueue_write_buffer(Buffer& buffer, vector<u32>& src, bool blocking) {
    ZoneScopedN("CommandQueue_write_buffer");
    TT_ASSERT(not blocking, "EnqueueWriteBuffer only has support for non-blocking mode currently");
    uint32_t src_size_bytes = src.size() * sizeof(uint32_t);
    TT_ASSERT(
        src_size_bytes <= buffer.size(),
        "Bounds-Error -- Attempting to write {} bytes to a {} byte buffer",
        src_size_bytes,
        buffer.size());
    TT_ASSERT(
        buffer.page_size() < MEM_L1_SIZE - DeviceCommand::DATA_SECTION_ADDRESS,
        "Buffer pages must fit within the command queue data section");

    u32 write_buffer_command_size = DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND + buffer.size();
    if ((this->sysmem_writer.cq_write_interface.fifo_wr_ptr << 4) + write_buffer_command_size >= HUGE_PAGE_SIZE) {
        tt::log_assert(
            write_buffer_command_size <= HUGE_PAGE_SIZE - 96,
            "EnqueueWriteBuffer command is too large: {}",
            write_buffer_command_size);
        this->wrap();
    }
    tt::log_debug(tt::LogDispatch, "EnqueueWriteBuffer");

    // TODO(agrebenisan): This could just be a stack variable since we
    // are just running in one thread
    shared_ptr<EnqueueWriteBufferCommand> command =
        std::make_shared<EnqueueWriteBufferCommand>(this->device, buffer, src, this->sysmem_writer);
    this->enqueue_command(command, blocking);
}

void CommandQueue::enqueue_program(Program& program, bool blocking) {
    ZoneScopedN("CommandQueue_enqueue_program");
    TT_ASSERT(not blocking, "EnqueueProgram only has support for non-blocking mode currently");

    // Need to relay the program into DRAM if this is the first time
    // we are seeing it
    const u64 program_id = program.get_id();

    // Whether or not we should stall the producer from prefetching binary data. If the
    // data is cached, then we don't need to stall, otherwise we need to wait for the
    // data to land in DRAM first
    bool stall = false;
    if (not this->program_to_buffer.count(program_id)) {
        stall = true;
        ProgramMap program_to_device_map = ConstructProgramMap(this->device, program);

        vector<u32>& program_pages = program_to_device_map.program_pages;
        u32 program_data_size_in_bytes = program_pages.size() * sizeof(u32);

        u32 write_buffer_command_size = DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND + program_data_size_in_bytes;

        this->program_to_buffer.emplace(
            program_id,
            std::make_unique<Buffer>(
                this->device, program_data_size_in_bytes, DeviceCommand::PROGRAM_PAGE_SIZE, BufferType::DRAM));

        this->enqueue_write_buffer(*this->program_to_buffer.at(program_id), program_pages, blocking);
        this->program_to_dev_map.emplace(program_id, std::move(program_to_device_map));
    }

    tt::log_debug(tt::LogDispatch, "EnqueueProgram");
    vector<u32> host_data;

    // Writing runtime args and circular buffer configs
    constexpr static u32 padding_alignment = 16;
    for (const auto kernel_id : program.kernel_ids()) {
        const Kernel* kernel = detail::GetKernel(program, kernel_id);
        for (const auto& [_, core_runtime_args] : kernel->runtime_args()) {
            host_data.insert(host_data.end(), core_runtime_args.begin(), core_runtime_args.end());
            u32 num_padding = align(host_data.size(), padding_alignment / sizeof(u32)) - host_data.size();
            for (u32 i = 0; i < num_padding; i++) {
                host_data.push_back(0);
            }
        }
    }

    for (const shared_ptr<CircularBuffer>& cb : program.circular_buffers()) {
        for (const auto buffer_index : cb->buffer_indices()) {
            host_data.push_back(cb->address() >> 4);
            host_data.push_back(cb->size() >> 4);
            host_data.push_back(cb->num_pages(buffer_index));
            host_data.push_back((cb->size() / cb->num_pages(buffer_index)) >> 4);
        }
    }

    u32 host_data_and_device_command_size =
        DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND + (host_data.size() * sizeof(u32));

    if ((this->sysmem_writer.cq_write_interface.fifo_wr_ptr << 4) + host_data_and_device_command_size >=
        HUGE_PAGE_SIZE) {
        tt::log_assert(
            host_data_and_device_command_size <= HUGE_PAGE_SIZE - 96, "EnqueueProgram command size too large");
        this->wrap();
    }

    shared_ptr<EnqueueProgramCommand> command = std::make_shared<EnqueueProgramCommand>(
        this->device,
        *this->program_to_buffer.at(program_id),
        this->program_to_dev_map.at(program_id),
        this->sysmem_writer,
        host_data,
        stall
        );

    this->enqueue_command(command, blocking);
}

void CommandQueue::finish() {
    ZoneScopedN("CommandQueue_finish");
    if ((this->sysmem_writer.cq_write_interface.fifo_wr_ptr << 4) + DeviceCommand::NUM_BYTES_IN_DEVICE_COMMAND >=
        HUGE_PAGE_SIZE) {
        this->wrap();
    }
    tt::log_debug(tt::LogDispatch, "Finish");

    FinishCommand command(this->device, this->sysmem_writer);
    shared_ptr<FinishCommand> p = std::make_shared<FinishCommand>(std::move(command));
    this->enqueue_command(p, false);

    // We then poll to check that we're done.
    vector<u32> finish_vec;
    do {
        tt::Cluster::instance().read_sysmem_vec(finish_vec, HOST_CQ_FINISH_PTR, 4, 0);
    } while (finish_vec.at(0) != 1);

    // Reset this value to 0 before moving on
    finish_vec.at(0) = 0;
    tt::Cluster::instance().write_sysmem_vec(finish_vec, HOST_CQ_FINISH_PTR, 0);
}

void CommandQueue::wrap() {
    ZoneScopedN("CommandQueue_wrap");
    tt::log_debug(tt::LogDispatch, "EnqueueWrap");
    EnqueueWrapCommand command(this->device, this->sysmem_writer);
    shared_ptr<EnqueueWrapCommand> p = std::make_shared<EnqueueWrapCommand>(std::move(command));
    this->enqueue_command(p, false);
}

// OpenCL-like APIs
void EnqueueReadBuffer(CommandQueue& cq, Buffer& buffer, vector<u32>& dst, bool blocking) {
    detail::DispatchStateCheck(true);
    TT_ASSERT(blocking, "Non-blocking EnqueueReadBuffer not yet supported");
    cq.enqueue_read_buffer(buffer, dst, blocking);
}

void EnqueueWriteBuffer(CommandQueue& cq, Buffer& buffer, vector<u32>& src, bool blocking) {
    detail::DispatchStateCheck(true);
    cq.enqueue_write_buffer(buffer, src, blocking);
}

void EnqueueProgram(CommandQueue& cq, Program& program, bool blocking) {
    detail::DispatchStateCheck(true);

    detail::CompileProgram(cq.device, program);

    program.allocate_circular_buffers();
    detail::ValidateCircularBufferRegion(program, cq.device);
    cq.enqueue_program(program, blocking);
}

void Finish(CommandQueue& cq) {
    detail::DispatchStateCheck(true);
    cq.finish();
}

}  // namespace tt::tt_metal
