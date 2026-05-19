FROM debian:stable-slim

RUN apt-get update && apt-get install -y \
    gcc \
    libwebsockets-dev \
    libssl-dev

WORKDIR /app


COPY . .

RUN gcc ./src/_socket.c -o socket -lwebsockets

CMD ["./socket"]