from pathlib import Path
import sys
f = f"{Path(__file__).parent}"
sys.path.append(f"{f}/..")

import torch
from transformers import BertTokenizer, BertForQuestionAnswering

import ll_buda_bindings.ll_buda_bindings._C as _C
from utility_functions import pad_activation, pad_weight, tilize_to_list, untilize, print_diff_argmax


# Initialize the device
device = _C.device.CreateDevice(_C.device.Arch.GRAYSKULL, 0)
_C.device.InitializeDevice(device)
host = _C.device.GetHost()

def feed_forward(ffn_dim, hidden_dim, ff1_weighta, ff1_biasa, ff2_weighta, ff2_biasa):

    # FF1 init
    ff1_weight = _C.tensor.Tensor(
        ff1_weighta,
        [1, 1, ffn_dim, hidden_dim],
        _C.tensor.DataFormat.FLOAT32,
        _C.tensor.Layout.TILE,
        device
    )
    ff1_bias = _C.tensor.Tensor(
        ff1_biasa,
        [1, 1, 32, ffn_dim],
        _C.tensor.DataFormat.FLOAT32,
        _C.tensor.Layout.TILE,
        device
    )
    ff1_out_activation_fn = _C.tensor.gelu

    # FF2 init
    ff2_weight = _C.tensor.Tensor(
        ff2_weighta,
        [1, 1, hidden_dim, ffn_dim],
        _C.tensor.DataFormat.FLOAT32,
        _C.tensor.Layout.TILE,
        device
    )
    ff2_bias = _C.tensor.Tensor(
        ff2_biasa,
        [1, 1, 32, hidden_dim],
        _C.tensor.DataFormat.FLOAT32,
        _C.tensor.Layout.TILE,
        device
    )

    def feed_forward_(activation):
        # ff1
        ff1_weight_T = _C.tensor.transpose(ff1_weight)
        ff1_output = _C.tensor.matmul(activation, ff1_weight_T)
        ff1_output_plus_bias = _C.tensor.bcast(ff1_output, ff1_bias, _C.tensor.BcastOpMath.ADD, _C.tensor.BcastOpDim.H)
        ff1_output_plus_bias_act = ff1_out_activation_fn(ff1_output_plus_bias)

        # ff2
        ff2_weight_T = _C.tensor.transpose(ff2_weight)
        ff2_output = _C.tensor.matmul(ff1_output_plus_bias_act, ff2_weight_T)
        ff2_output_plus_bias = _C.tensor.bcast(ff2_output, ff2_bias, _C.tensor.BcastOpMath.ADD, _C.tensor.BcastOpDim.H)
        
        return ff2_output_plus_bias

    return feed_forward_

class TtFeedForwardModel(torch.nn.Module):
    def __init__(self, state_dict):
        super().__init__()

        # FF1 params
        encoder0_ff1_weight = pad_weight(state_dict["bert.encoder.layer.0.intermediate.dense.weight"])
        encoder0_ff1_bias = pad_weight(state_dict["bert.encoder.layer.0.intermediate.dense.bias"])
        
        encoder0_ff1_weight_shape = encoder0_ff1_weight.shape
        encoder0_ff1_bias_shape = encoder0_ff1_bias.shape

        encoder0_ff1_weight = tilize_to_list(encoder0_ff1_weight)
        encoder0_ff1_bias = tilize_to_list(encoder0_ff1_bias)

        # FF2 params
        encoder0_ff2_weight = pad_weight(state_dict["bert.encoder.layer.0.output.dense.weight"])
        encoder0_ff2_bias = pad_weight(state_dict["bert.encoder.layer.0.output.dense.bias"])
    
        encoder0_ff2_weight_shape = encoder0_ff2_weight.shape
        encoder0_ff2_bias_shape = encoder0_ff2_bias.shape

        encoder0_ff2_weight = tilize_to_list(encoder0_ff2_weight)
        encoder0_ff2_bias = tilize_to_list(encoder0_ff2_bias)

        self.ffn = feed_forward(
            *encoder0_ff1_weight_shape[-2:], 
            encoder0_ff1_weight, 
            encoder0_ff1_bias, 
            encoder0_ff2_weight, 
            encoder0_ff2_bias
        )
    
    def forward(self, x):
        tilized_x = tilize_to_list(pad_activation(x))
        inp = _C.tensor.Tensor(tilized_x, x.shape, _C.tensor.DataFormat.FLOAT32,  _C.tensor.Layout.TILE, device)

        ffn_output = self.ffn(inp)
        return ffn_output

class PytorchFeedForwardModel(torch.nn.Module):
    def __init__(self, hugging_face_reference_model):
        super().__init__()
        self.ff1 = hugging_face_reference_model.bert.encoder.layer[0].intermediate
        self.ff2 = hugging_face_reference_model.bert.encoder.layer[0].output.dense
        
    def forward(self, x):
        return self.ff2(self.ff1(x))

def summarize_stats(t, name):
    mean = t.mean()
    std = t.std()
    mag = t.norm()
    max = t.max()
    print(f"STATS FOR {name}")
    print(f"mean {mean}")
    print(f"std {std}")
    print(f"mag {mag}")
    print(f"max {max}")
    print()

def run_ffn_inference():
    hugging_face_reference_model = BertForQuestionAnswering.from_pretrained("prajjwal1/bert-tiny", torchscript=False)
    tt_ffn_model = TtFeedForwardModel(hugging_face_reference_model.state_dict())
    pytorch_ffn_model = PytorchFeedForwardModel(hugging_face_reference_model)

    # Prepare input
    torch.manual_seed(0)
    ffn_input = (torch.rand(1, 1, 128, 128) * 2) - 1

    pytorch_out = pytorch_ffn_model(ffn_input)
    tt_out = tt_ffn_model(ffn_input).to(host)
    tt_out = untilize(torch.Tensor(tt_out.data()).reshape(*pytorch_out.shape))

    # Summarizing weight statistics
    print("Summarizing stats for weights")
    state_dict = hugging_face_reference_model.state_dict()
    summarize_stats(state_dict["bert.encoder.layer.0.intermediate.dense.weight"], "ff1 weight")
    summarize_stats(state_dict["bert.encoder.layer.0.intermediate.dense.bias"], "ff1 bias")
    summarize_stats(state_dict["bert.encoder.layer.0.output.dense.weight"], "ff2 weight")
    summarize_stats(state_dict["bert.encoder.layer.0.output.dense.weight"], "ff2 bias")

    # Summarize output statistics
    print("Summarizing stats for outputs")
    summarize_stats(pytorch_out, "pytorch output")
    summarize_stats(tt_out, "tt output")
    summarize_stats(abs(pytorch_out - tt_out), "absolute difference in outputs")

    
    return 

if __name__ == "__main__":
    run_ffn_inference()

_C.device.CloseDevice(device)