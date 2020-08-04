#
# First stage - builder
#
FROM ubuntu:eoan AS builder

ENV DEBIAN_FRONTEND noninteractive
ENV LC_ALL C.UTF-8

# This allows you to use a local Ubuntu mirror
ARG APT_URL=
ENV APT_URL ${APT_URL:-http://archive.ubuntu.com/ubuntu/}
RUN sed -i "s%http://archive.ubuntu.com/ubuntu/%${APT_URL}%" /etc/apt/sources.list

# Update packages and install dependencies
RUN apt-get update && apt-get -y upgrade \
    && apt-get install -y protobuf-compiler libh2o-dev libcurl4-openssl-dev \
           libssl-dev libprotobuf-dev libh2o-evloop-dev libwslay-dev \
           libeigen3-dev libzstd-dev libfmt-dev libncurses5-dev \
	       make gcc g++ git build-essential curl autoconf automake help2man

# Build
ARG MAKE_FLAGS=-j2
ADD . /galmon-src/
WORKDIR /galmon-src/
RUN make $MAKE_FLAGS
RUN prefix=/galmon make install

#
# Second stage - contains just the binaries
#
FROM ubuntu:eoan
RUN apt-get update && apt-get -y upgrade \
    && apt-get install -y libh2o0.13 libcurl4 libssl1.1 libprotobuf17 \
           libh2o-evloop0.13 libwslay1 libzstd1 \
    && apt-get -y clean
COPY --from=builder /galmon/ /galmon/
WORKDIR /galmon/bin
ENV PATH=/galmon/bin:${PATH}
