import torch


################################################
#################### TT-DNN ####################
################################################
def datacopy(x, *args, **kwargs):
    return x


def recip(x, *args, **kwargs):
    return torch.reciprocal(x)


def exp(x, *args, **kwargs):
    return torch.exp(x)


def sqrt(x, *args, **kwargs):
    return torch.sqrt(x)


def gelu(x, *args, **kwargs):
    return torch.nn.functional.gelu(x)


def relu(x, *args, **kwargs):
    return torch.nn.functional.relu(x)


def sigmoid(x, *args, **kwargs):
    return torch.nn.functional.sigmoid(x)


def log(x, *args, **kwargs):
    return torch.log(x)


def tanh(x, *args, **kwargs):
    return torch.tanh(x)


def add(x, y, *args, **kwargs):
    return torch.add(x, y)


def sub(x, y, *args, **kwargs):
    return torch.sub(x, y)


def mul(x, y, *args, **kwargs):
    return torch.mul(x, y)


def matmul(x, y, *args, **kwargs):
    return torch.matmul(x, y)


def reduce_sum(x, dims=None, keepdim=True, *args, **kwargs):
    return torch.sum(x, dims, keepdim)


def reduce_max(x, dims=None, keepdim=True, *args, **kwargs):
    return torch.amax(x, dims, keepdim)


def flatten(x, *args, **kwargs):
    return torch.flatten(x)


def transpose(x, dim0=-2, dim1=-1, *args, **kwargs):
    return torch.transpose(x, dim0, dim1)


def permute(x, *args, **kwargs):
    assert "permute_dims" in kwargs
    permute_dims = kwargs["permute_dims"]
    return torch.permute(x, permute_dims)


def reshape(x, *args, **kwargs):
    assert "reshape_dims" in kwargs
    reshape_dims = kwargs["reshape_dims"]
    return torch.reshape(x, reshape_dims)


################################################
#################### Tensor ####################
################################################
def pad(x, *args, **kwargs):
    assert "output_tensor_shape" in kwargs
    assert "input_tensor_start" in kwargs
    assert "pad_value" in kwargs

    output_tensor_shape = kwargs["output_tensor_shape"]
    input_tensor_start = kwargs["input_tensor_start"]
    pad_value = kwargs["pad_value"]

    input_tensor_shape = x.shape
    input_tensor_end = tuple(
        input_tensor_start[i] + input_tensor_shape[i]
        for i in range(len(input_tensor_shape))
    )
    out = torch.ones(*output_tensor_shape, dtype=torch.bfloat16) * pad_value
    out[
        input_tensor_start[0] : input_tensor_end[0],
        input_tensor_start[1] : input_tensor_end[1],
        input_tensor_start[2] : input_tensor_end[2],
        input_tensor_start[3] : input_tensor_end[3],
    ] = x

    return out


def unpad(x, *args, **kwargs):
    assert "output_tensor_start" in kwargs
    assert "output_tensor_end" in kwargs

    output_tensor_start = kwargs["output_tensor_start"]
    output_tensor_end = kwargs["output_tensor_end"]

    out = x[
        output_tensor_start[0] : output_tensor_end[0] + 1,
        output_tensor_start[1] : output_tensor_end[1] + 1,
        output_tensor_start[2] : output_tensor_end[2] + 1,
        output_tensor_start[3] : output_tensor_end[3] + 1,
    ]

    return out
