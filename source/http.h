const static char http_200[] = "HTTP/1.1 200 OK\r\n";
const static char http_201[] = "HTTP/1.1 201 Created\r\n";

const static char indexdata[] = "<html> <head><body> \
	<form method=\"POST\" action=\".\" enctype=\"multipart/form-data\">\
			file: <input type=\"file\" name=\"file\">\
			<input type=\"submit\">\
		</form> \           
	</body> \
	</html>";

const static char http_html_hdr[] = "Content-type: text/html\r\n\r\n";
const static char http_get_index[] = "GET / HTTP/1.1\r\n";
const static char http_get_list[] = "GET /list HTTP/1.1\r\n";
const static char http_post_index[] = "POST / HTTP/1.1\r\n";