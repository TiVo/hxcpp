# Use same distribution as Travis.CI
FROM ubuntu:12.04

# Install all dependencies to build hxcpp, haxe and neko
RUN apt-get update
RUN apt-get install -y \
                         git \
                         sudo \
                         build-essential \
                         libpcre3-dev \
                         ocaml-native-compilers \
                         zlib1g-dev \
                         libgc-dev \
                         gcc-multilib \
                         g++-multilib

# Build neko
RUN git clone https://github.com/HaxeFoundation/neko.git /neko
WORKDIR /neko
RUN make
RUN sudo make install

# Build haxe
RUN git clone --recursive https://github.com/HaxeFoundation/haxe.git /haxe
WORKDIR /haxe
RUN make OCAMLOPT=ocamlopt.opt ADD_REVISION=1
RUN make tools
RUN make install

# Mount point for hxcpp code
VOLUME /hxcpp

# Setup haxelib with hxcpp
RUN mkdir /haxelib
WORKDIR /haxelib
RUN haxelib setup .
RUN haxelib dev hxcpp /hxcpp

# Copy hxcpp test script
RUN mkdir /script
COPY ./run_tests.sh /script/

# Add env needed by hxcpp test script
ENV BUILD_DIR /hxcpp
ENV HAXE_DIR /haxe

# Set the default dir when running the container
WORKDIR /script
CMD ./run_tests.sh

