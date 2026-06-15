import argparse
import collections
import re

import onnx


def parse_arg():
    """解析命令行参数，获取ONNX模型和RKNN算子支持表路径。"""
    parser = argparse.ArgumentParser(description="Inspect ONNX operators against RKNN Toolkit2 support table.")
    parser.add_argument("onnx_model_path", help="ONNX model path")
    parser.add_argument("op_support_doc", help="RKNN Toolkit2 OP support markdown path")
    return parser.parse_args()


def load_onnx_support_table(doc_path):
    """读取RKNN算子支持表，提取ONNX算子的支持状态。"""
    support_table = {}
    in_onnx_section = False

    with open(doc_path, "r", encoding="utf-8") as doc_file:
        for line in doc_file:
            if line.startswith("## ONNX OPs supported"):
                in_onnx_section = True
                continue

            if in_onnx_section and line.startswith("## "):
                break

            match = re.match(r"^\|\s*([^|]+?)\s*\|\s*([^|]*?)\s*\|", line)
            if match is None:
                continue

            op_name = match.group(1).strip()
            remark = match.group(2).strip()

            if op_name == "**Operators**" or set(op_name) <= set("- "):
                continue

            support_table[op_name] = remark

    return support_table


def get_tensor_shape(value_info):
    """读取ONNX张量形状，动态维度使用问号表示。"""
    dims = []
    tensor_type = value_info.type.tensor_type

    for dim in tensor_type.shape.dim:
        dims.append(dim.dim_param or dim.dim_value or "?")

    return dims


def print_model_io(model):
    """打印ONNX模型输入和输出张量信息。"""
    print("\nINPUTS:")
    for value in model.graph.input:
        print("- {}: {}".format(value.name, get_tensor_shape(value)))

    print("\nOUTPUTS:")
    for value in model.graph.output:
        print("- {}: {}".format(value.name, get_tensor_shape(value)))


def print_op_report(model, support_table):
    """打印模型算子统计，并标记不支持或有限制的算子。"""
    op_counter = collections.Counter(node.op_type for node in model.graph.node)
    unsupported_ops = []
    remarked_ops = []
    unknown_ops = []

    print("\nOPS:")
    for op_name, count in sorted(op_counter.items()):
        remark = support_table.get(op_name)

        if remark is None:
            unknown_ops.append((op_name, count))
            remark = "UNKNOWN_IN_SUPPORT_DOC"
        elif "Not Supported" in remark:
            unsupported_ops.append((op_name, count, remark))
        elif remark:
            remarked_ops.append((op_name, count, remark))

        print("- {}: {} | {}".format(op_name, count, remark))

    print("\nUNSUPPORTED:")
    if unsupported_ops:
        for op_name, count, remark in unsupported_ops:
            print("- {}: {} | {}".format(op_name, count, remark))
    else:
        print("NONE")

    print("\nLIMITED_OR_REMARKED:")
    if remarked_ops:
        for op_name, count, remark in remarked_ops:
            print("- {}: {} | {}".format(op_name, count, remark))
    else:
        print("NONE")

    print("\nUNKNOWN:")
    if unknown_ops:
        for op_name, count in unknown_ops:
            print("- {}: {}".format(op_name, count))
    else:
        print("NONE")


def print_tail_nodes(model, count):
    """打印ONNX模型末尾节点，便于检查后处理是否仍在模型内。"""
    print("\nLAST_{}_NODES:".format(count))
    nodes = list(enumerate(model.graph.node))[-count:]

    for index, node in nodes:
        print("{}: {} name={} outputs={}".format(index, node.op_type, node.name, list(node.output)))


def main():
    """加载ONNX模型和支持表，输出静态兼容性检查结果。"""
    args = parse_arg()
    model = onnx.load(args.onnx_model_path)
    support_table = load_onnx_support_table(args.op_support_doc)

    print("MODEL: {}".format(args.onnx_model_path))
    print("IR_VERSION: {}".format(model.ir_version))
    print("OPSET: {}".format(", ".join("{}:{}".format(item.domain or "ai.onnx", item.version) for item in model.opset_import)))

    print_model_io(model)
    print_op_report(model, support_table)
    print_tail_nodes(model, 20)


if __name__ == "__main__":
    main()
