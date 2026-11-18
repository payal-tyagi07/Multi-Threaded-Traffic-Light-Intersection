# Use official Ubuntu image as base
FROM ubuntu:22.04

# Install build essentials (g++, make) and python3 (for a lightweight web server)
RUN apt-get update && apt-get install -y \
    build-essential \
    python3 \
    && rm -rf /var/lib/apt/lists/*

# Set working directory inside the container
WORKDIR /app

# Copy project files into the container
COPY . /app

# Compile the C++ POSIX backend using your Makefile
RUN make

# Expose port for the web dashboard
EXPOSE 8080

# Command to run a lightweight Python HTTP server for index.html 
# (Note: For live terminal log streaming, you can run the C++ binary or view logs via cloud terminal)
CMD ["python3", "-m", "http.server", "8080"]