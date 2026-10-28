# One Dockerfile for blue, gateway and red:  docker build --build-arg SERVICE=gateway .
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY include include
COPY blue blue
COPY gateway gateway
COPY red red
COPY tests tests
COPY Makefile .
ARG SERVICE
RUN make bin/${SERVICE}

FROM debian:bookworm-slim
ARG SERVICE
WORKDIR /app
COPY --from=build /src/bin/${SERVICE} /app/service
COPY gateway/static /app/static
CMD ["/app/service"]
