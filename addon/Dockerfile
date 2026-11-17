ARG BUILD_FROM
FROM $BUILD_FROM

# Standard library only: no pip packages
RUN apk add --no-cache python3

ARG BUILD_VERSION
ENV ADDON_VERSION=${BUILD_VERSION}

COPY server.py /app/server.py
COPY www /app/www

CMD [ "python3", "-u", "/app/server.py" ]
