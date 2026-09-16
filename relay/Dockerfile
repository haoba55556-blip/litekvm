# SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
# SPDX-License-Identifier: GPL-2.0
FROM golang:1.22-alpine AS build
WORKDIR /src
COPY main.go .
RUN CGO_ENABLED=0 go build -ldflags="-s -w" -o /litekvm-relay .

FROM alpine:3.20
RUN adduser -D -u 10001 relay
COPY --from=build /litekvm-relay /usr/local/bin/litekvm-relay
USER relay
EXPOSE 25910
ENTRYPOINT ["litekvm-relay"]
