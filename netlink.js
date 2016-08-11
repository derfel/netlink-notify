"use strict"; 

const worker = require("streaming-worker");
const path = require("path");

let addon_path = path.join(__dirname, "build/Release/netlink_worker");

const netlink = worker(addon_path);

module.exports = netlink;

