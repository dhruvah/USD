#!/bin/bash
docker build --network=host -f 21.05/Dockerfile -t ubuntu-focal-usd-monolithic:21.05 -t ubuntu-focal-usd-monolithic:latest ../
