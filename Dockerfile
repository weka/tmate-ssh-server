FROM alpine:3.20 AS deps 

RUN apk add --no-cache msgpack-c ncurses-libs libevent openssl zlib

RUN apk add --no-cache \
	autoconf \
	automake \
	cmake \
	g++ \
	gcc \
	git \
	libevent \
	libevent-dev \
	libssh-dev \
	linux-headers \
	make \
	msgpack-c \
	msgpack-c-dev \
	ncurses-dev \
	ncurses-libs \
	openssl \
	openssl-dev \
	zlib \
	zlib-dev

FROM deps AS nats_build

RUN git clone https://github.com/nats-io/nats.c.git /src/cnats;

RUN set -ex; \ 
	cd /src/cnats; \
	git checkout v3.9.0; \
	mkdir build; \
	cd build; \
	cmake .. -DNATS_BUILD_STREAMING=OFF -DNATS_BUILD_EXAMPLES=OFF -DBUILD_TESTING:BOOL=OFF -DCMAKE_INSTALL_PREFIX:PATH=/usr; \
	cmake --build . --target install --config Release

FROM nats_build AS build

WORKDIR /src/tmate-ssh-server
COPY . /src/tmate-ssh-server

RUN set -ex; \
	./autogen.sh; \
	./configure --prefix=/usr CFLAGS="-D_GNU_SOURCE"; \
	make -j "$(nproc)"; \
	make install

### Minimal run-time image
FROM alpine:3.20

RUN apk add --no-cache \
	bash \
	gdb \
	libevent \
	libssh \
	msgpack-c \
	ncurses-libs \
	openssl \
	zlib

COPY --from=build /usr/bin/tmate-ssh-server /usr/bin/
COPY --from=nats_build /usr/lib/libnats.so.3.9 /usr/lib

# TODO not run as root. Instead, use capabilities.

COPY docker-entrypoint.sh /usr/local/bin

EXPOSE 2200
ENTRYPOINT ["docker-entrypoint.sh"]
