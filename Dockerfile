# ---- build stage: compile the C17 server with gcc ----
FROM gcc:14 AS build
WORKDIR /src
COPY Makefile ./
COPY src ./src
# Static binary: the runtime image needs nothing but this one file.
RUN make CFLAGS="-O2" LDFLAGS="-static -pthread" && strip build/server

# ---- runtime stage: small image, non-root user ----
FROM alpine:3.20
RUN addgroup -S -g 10001 app && adduser -S -D -H -u 10001 -G app app
WORKDIR /app
COPY --from=build /src/build/server /app/server
COPY public /app/public
USER 10001:10001
ENV PORT=8080 DOCROOT=/app/public
EXPOSE 8080
CMD ["/app/server"]
