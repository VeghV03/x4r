#!/bin/bash
# The cloud environment's setup script (paste it into the environment's Setup script field).
# Runs as root on Ubuntu 24.04 before a run; the result is cached for about a week.
# Python, uv, git and Node 22 come with the image. The news engine reads the Magyar Közlöny's
# PDFs with pdftotext.
set -e
apt-get update -qq
apt-get install -y -qq poppler-utils
