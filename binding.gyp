{
	"targets": [
		{
			"target_name": "netlink_worker",
			"sources": [ "netlink.cpp" ], 
			"cflags": ["-Wall", "-std=c++11"],
			"cflags_cc": ["-Wall", "-std=c++11"],
			"cflags!": [ "-fno-exceptions" ],
			"cflags_cc!": [ "-fno-exceptions" ],
			"ldflags": ["-lmnl", "-std=c++11"],
			"include_dirs" : [
				"<!(node -e \"require('nan')\")", 
				"<!(node -e \"require('streaming-worker-sdk')\")"
			]
		}
	]
}
