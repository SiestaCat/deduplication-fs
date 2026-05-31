FROM gcc:13-bookworm AS build

WORKDIR /app

COPY Makefile ./
COPY src ./src

RUN make

FROM debian:bookworm-slim

WORKDIR /app

COPY --from=build /app/app ./app

CMD ["./app"]
