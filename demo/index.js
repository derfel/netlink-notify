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

setTimeout(() => {
	netlink.to.emit('get_route', 'ipv4');
}, 1000);
setInterval(() => {
	netlink.to.emit('get_addr', 'ipv4');
}, 2000);
setTimeout(() => {
	netlink.to.emit('get_link');
}, 3000);

