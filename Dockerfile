FROM ubuntu:20.04
RUN apt-get -y update && apt-get install -y
RUN DEBIAN_FRONTEND=noninteractive apt-get --no-install-recommends -y install clang cmake build-essential

COPY stormlib /root/stormlib

WORKDIR /root/stormlib
RUN cmake CMakeLists.txt && make && make install

COPY . /root/
WORKDIR /root/
RUN make

CMD ["./bin/s2parser", "--http"]
