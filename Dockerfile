# 1. 使用一个轻量、稳定的 Ubuntu 镜像作为基础
FROM ubuntu:22.04

# 2. 更新系统并安装 C++ 开发所需的核心工具包
#    build-essential: 包含了 g++, gcc, make 等编译器
#    gdb: 强大的 C++ 调试器，为您以后调试代码做准备
RUN apt-get update && apt-get install -y \
    build-essential \
    gdb \
    git \
    vim \
    tree \
    mingw-w64 


# 3. 设置工作目录
WORKDIR /app