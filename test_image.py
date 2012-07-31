import httplib

def test_post():
	f = open('big.jpg', 'r')
	data = f.read()
	f.close()

	conn = httplib.HTTPConnection("192.168.1.35:81")

	headers = {
		"Content-Length": len(data),
		"X-ImageMagick-Convert": "-strip -quality 80 -resize 500x500>"
	}
	conn.request("POST", "/magickd/", data, headers)

	rsp = conn.getresponse()

	print 'Status %s %s' % (rsp.status, rsp.reason)
	for h in rsp.getheaders():
		print '%s: %s' % (h[0], h[1])

	if rsp.status == 200:
		data = rsp.read()
		f = open('converted_post1.jpg', 'wb')
		f.write(data)
		f.close()

def test_get():
	conn = httplib.HTTPConnection("192.168.1.35:81")

	# X-ImageMagick-Convert=/testdata/testimage.jpg -strip -resize 500x500>

	headers = {
		"X-ImageMagick-Convert": "/fw554.jpg -strip -resize 500x500>"
	}
	conn.request("GET", "/magickd/", None, headers)

	rsp = conn.getresponse()

	print 'Status %s %s' % (rsp.status, rsp.reason)
	for h in rsp.getheaders():
		print '%s: %s' % (h[0], h[1])

	if rsp.status == 200:
		data = rsp.read()
		f = open('converted_get.jpg', 'wb')
		f.write(data)
		f.close()
	

if __name__ == '__main__':
	test_get()
	#test_post()
