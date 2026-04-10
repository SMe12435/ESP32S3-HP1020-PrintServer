FROM python:3.12-slim

RUN apt-get update && \
    apt-get install -y --no-install-recommends libmupdf-dev libjbig0 && \
    cd /tmp && apt-get download printer-driver-foo2zjs && \
    dpkg -x printer-driver-foo2zjs*.deb /tmp/foo2zjs-extract && \
    cp /tmp/foo2zjs-extract/usr/bin/foo2zjs /usr/local/bin/ && \
    chmod +x /usr/local/bin/foo2zjs && \
    rm -rf /tmp/foo2zjs* /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

RUN mkdir -p /app/jobs

EXPOSE 5000

CMD ["gunicorn", "--worker-class", "gthread", "--threads", "4", \
     "-w", "1", "-b", "0.0.0.0:5000", "--timeout", "120", "app:app"]
