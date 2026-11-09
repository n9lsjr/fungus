# Arena image for gridsnake.com (external_image project). The runner starts it
# read-only, cap_drop ALL, user 1000:1000, 512 MB, 1 CPU and connects to :50051.
FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libgrpc++1.51 libprotobuf32 \
 && rm -rf /var/lib/apt/lists/*
COPY csnake /app/csnake
USER 1000:1000
EXPOSE 50051
ENTRYPOINT ["/app/csnake", "0.0.0.0:50051"]
