"use strict"; 

let netlink = require('netlink-notify');

netlink.from.on('route', function(factor){
	console.log("route: " + factor);
});

netlink.from.on('link', function(factor){
	console.log("link: " + factor);
});

netlink.from.on('addr', function(factor){
	console.log("addr: " + factor);
});

netlink.from.on('error', function(e) {
	console.log(e);
});

