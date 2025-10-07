pixels = executable('pixels',
	[
		'test-pixels.c',
		'../src/pixels.c',
	],
	include_directories: inc,
	dependencies: [
		pixman,
		libdrm_inc,
		libm,
	],
)
test('pixels', pixels)

base64 = executable('base64',
	[
		'test-base64.c',
		'../src/base64.c',
	],
	include_directories: inc,
)
test('base64', base64)

stream_interface = executable('stream-interface',
	[
		'test-stream-interface.c',
		'../src/stream/common.c',
		'../src/stream/tcp.c',
		'../src/stream/interface.c',
		'../src/rcbuf.c',
		'../src/vec.c',
	],
	include_directories: include_directories('../include', '../src', '..'),
	dependencies: [
	  aml
	],
)
test('stream-interface', stream_interface)
