# Netlink Notify
[![npm version](https://badge.fury.io/js/netlink-notify.svg)](http://badge.fury.io/js/netlink-notify)
[![Dependencies Status](https://david-dm.org/derfel/netlink-notify.svg)](https://david-dm.org/derfel/netlink-notify)
[![Known Vulnerabilities](https://snyk.io/test/npm/netlink-notify/badge.svg)](https://snyk.io/test/npm/netlink-notify)

A module to get netlink notification event on Linux.

## Notification supported:
* route
* link
* address

This module will generate an event for nodejs for every event it receives from netlink.

## Depends:
* libmnl >= 1.0.0
* JSON for Modern C++ >= 2.0.0 (Already included from https://github.com/nlohmann/json)

## Installation

    npm install netlink-notify

## Build and run

The netlink-notify library is dependent on libmnl library, so you may need to install libmnl before compiling.
On Debian, and Debian derivates like Ubuntu, you need to install the package "libmnl-dev".

To build, you need the node package node-gyp:

	npm install -g node-gyp

After you have all of the prerequisite packages installed, you can build avro-nodejs.

	node-gyp configure build

There's a demo inside the demo/ dir.

## Status
Status: Alpha. Use at your own risk.

## Have questions? Found a bug?

Please submit issues to the Github issue tracker
