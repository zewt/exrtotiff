exrtotiff: exrtotiff.cpp
	g++ exrtotiff.cpp -o exrtotiff -I/usr/include/OpenEXR -lIlmImf -std=c++11 -ltiff -g -O2 -Wall

all: exrtotiff

