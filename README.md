[![Build Status](https://travis-ci.org/litespeedtech/lsquic.svg?branch=master)](https://travis-ci.org/litespeedtech/lsquic)
[![Build Status](https://api.cirrus-ci.com/github/litespeedtech/lsquic.svg)](https://cirrus-ci.com/github/litespeedtech/lsquic)
[![Build status](https://ci.appveyor.com/api/projects/status/kei9649t9leoqicr?svg=true)](https://ci.appveyor.com/project/litespeedtech/lsquic)

LiteSpeed QUIC (LSQUIC) Library README
=============================================

Description
-----------

LiteSpeed QUIC (LSQUIC) Library is an open-source implementation of QUIC
and HTTP/3 functionality for servers and clients.  Most of the code in this
distribution is used in our own products: LiteSpeed Web Server, LiteSpeed ADC,
and OpenLiteSpeed.  We think it is free of major problems.  Nevertheless, do
not hesitate to report bugs back to us.  Even better, send us fixes and
improvements!

Currently supported QUIC versions are Q039, Q043, Q046, ID-23, and ID-24.
Support for newer versions will be added soon after they are released.

Documentation
-------------

The documentation for this module is admittedly sparse.  The API is
documented in include/lsquic.h.  If you have doxygen, you can run
`doxygen dox.cfg` or `make docs`.  The example program is
test/http_client.c: a bare-bones, but working, QUIC client.  Have a look
in EXAMPLES.txt to see how it can be used.

Requirements
------------

To build LSQUIC, you need CMake, zlib, and BoringSSL.  The example program
uses libevent to provide the event loop.

Building BoringSSL
------------------

BoringSSL is not packaged; you have to build it yourself.  The process is
straightforward.  You will need `go` installed.

1. Clone BoringSSL by issuing the following command:

```
git clone https://github.com/raihan001/boringssl.git
cd boringssl
```

You may need to install pre-requisites like zlib and libevent.


2. Compile the library

```
==================================
For intel
==================================
cmake . &&  make
==================================
For ARM64
==================================
cmake EXTRA_CMAKE_OPTIONS='-DCMAKE_C_COMPILER=arm64-linux-gcc -DCMAKE_CXX_COMPILER=arm64-linux-gnu-g++ -DCXX_STANDARD_REQUIRED=c++17 -DCMAKE_POSITION_INDEPENDENT_CODE=on' . 
make -j4 EXTRA_CMAKE_OPTIONS='-DCMAKE_C_COMPILER=arm64-linux-gcc -DCMAKE_CXX_COMPILER=arm64-linux-gnu-g++ -DCXX_STANDARD_REQUIRED=c++17'

```

Remember where BoringSSL sources are:
```
BORINGSSL=$PWD
```

If you want to turn on optimizations, do

```
==================================
For intel
==================================
cmake -DCMAKE_BUILD_TYPE=Release . && make

==================================
For ARM64
==================================
cmake EXTRA_CMAKE_OPTIONS='-DCMAKE_C_COMPILER=arm64-linux-gcc -DCMAKE_CXX_COMPILER=arm64-linux-gnu-g++ -DCXX_STANDARD_REQUIRED=c++17 -DCMAKE_POSITION_INDEPENDENT_CODE=on' -DCMAKE_BUILD_TYPE=Release . 
make -j4 EXTRA_CMAKE_OPTIONS='-DCMAKE_C_COMPILER=arm64-linux-gcc -DCMAKE_CXX_COMPILER=arm64-linux-gnu-g++ -DCXX_STANDARD_REQUIRED=c++17'
```

Building LSQUIC Library
-----------------------

LSQUIC's `http_client`, `http_server`, and the tests link BoringSSL
libraries statically.  Following previous section, you can build LSQUIC
as follows:

1. Get the source code

```
git clone https://github.com/litespeedtech/lsquic.git
cd lsquic
git submodule init
git submodule update
```

2. Compile the library


```
# $BORINGSSL is the top-level BoringSSL directory from the previous step
cmake EXTRA_CMAKE_OPTIONS='-DCMAKE_C_COMPILER=arm64-linux-gcc -DCMAKE_CXX_COMPILER=arm64-linux-gnu-g++ -DCXX_STANDARD_REQUIRED=c++17 -DCMAKE_POSITION_INDEPENDENT_CODE=on' -DBORINGSSL_DIR=$BORINGSSL .
make -j4 EXTRA_CMAKE_OPTIONS='-DCMAKE_C_COMPILER=arm64-linux-gcc -DCMAKE_CXX_COMPILER=arm64-linux-gnu-g++ -DCXX_STANDARD_REQUIRED=c++17'
```

3. Run tests

```
make -j4 EXTRA_CMAKE_OPTIONS='-DCMAKE_C_COMPILER=arm64-linux-gcc -DCMAKE_CXX_COMPILER=arm64-linux-gnu-g++ -DCXX_STANDARD_REQUIRED=c++17' test
```


Have fun,

Copyright (c) 2017 - 2019 LiteSpeed Technologies Inc
