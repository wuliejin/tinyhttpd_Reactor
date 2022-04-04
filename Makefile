all:httpd

httpd: httpd_string.cpp
	g++ -o httpd_string httpd_string.cpp -lpthread

clean:
	rm httpd_string
