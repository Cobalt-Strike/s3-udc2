.PHONY: all clean debug

BOFS := s3-udc2.cpp s3-udc2-patchable.cpp s3-udc2-migrate.cpp

CC32 := i686-w64-mingw32-g++
CC64 := x86_64-w64-mingw32-g++

CXXFLAGS := -O2 -Wall -Wextra

OBJ64 := $(patsubst %.cpp,bofs/%.x64.o,$(BOFS))
OBJ32 := $(patsubst %.cpp,bofs/%.x86.o,$(BOFS))

all: bofs $(OBJ64) $(OBJ32)

bofs:
	mkdir -p $@

bofs/%.x64.o: %.cpp | bofs
	$(CC64) $(CXXFLAGS) -c $< -o $@

bofs/%.x86.o: %.cpp | bofs
	$(CC32) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf bofs

ifdef UDC2_DEBUG_HOST
UDC2_DEBUG_HOST_DEF := -DUDC2_DEBUG_HOST=\"$(UDC2_DEBUG_HOST)\"
else
UDC2_DEBUG_HOST_DEF :=
endif

ifdef UDC2_DEBUG_PORT
UDC2_DEBUG_PORT_DEF := -DUDC2_DEBUG_PORT=$(UDC2_DEBUG_PORT)
else
UDC2_DEBUG_PORT_DEF :=
endif

debug: bofs
	$(CC64) -o bofs/s3-udc2-dbg.exe \
		s3-udc2.cpp \
		base/mock.cpp \
		base/debug.cpp \
		-D_DEBUG $(UDC2_DEBUG_HOST_DEF) $(UDC2_DEBUG_PORT_DEF) \
		-lwinhttp -lrpcrt4 -lws2_32 -static -static-libgcc -static-libstdc++ \
		-g


