#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

#include <fcntl.h>

#include <sys/types.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <3ds.h>

#define DEBUG

#ifdef DEBUG
#define debug_print(...) printf(__VA_ARGS__)
#else
#define debug_print(...) 
#endif

#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000

static u32 *SOC_buffer = NULL;
s32 sock = -1, csock = -1;

__attribute__((format(printf, 1, 2))) void failExit(const char *fmt, ...);

const static char http_200[] = "HTTP/1.1 200 OK\r\n";
const static char http_201[] = "HTTP/1.1 201 Created\r\n";

const static char indexdata[] = "<html> <head><body> \
	<form method=\"POST\" action=\".\" enctype=\"multipart/form-data\">\
			file name: <input name=\"filename\"/><br/>\
			file: <input type=\"file\" name=\"file\">\
			<input type=\"submit\">\
		</form> \           
	</body> \
	</html>";

const static char http_html_hdr[] = "Content-type: text/html\r\n\r\n";
const static char http_get_index[] = "GET / HTTP/1.1\r\n";
const static char http_post_index[] = "POST / HTTP/1.1\r\n";

const int DEFAULT_READ_SIZE = 1024;

//---------------------------------------------------------------------------------
void socShutdown()
{
	//---------------------------------------------------------------------------------
	printf("waiting for socExit...\n");
	socExit();
}

//TODO it would be better to scan backwards as the end boundary is usually near the end of the request
const char *memmem(const char *haystack, size_t haystack_len, const char *needle, size_t needle_len)
{
	if (needle_len == 0)
		return haystack;
	while (needle_len <= haystack_len)
	{
		if (!memcmp(haystack, needle, needle_len))
			return haystack;
		haystack++;
		haystack_len--;
	}
	return NULL;
}

int find_line(char *dst, const char *haystack, const char *line_beginning)
{
	char *line = strstr(haystack, line_beginning);
	if (line == NULL)
		return -1;
	char *following_linebreak = strchr(line, '\r');
	if (following_linebreak == NULL)
	{
		//TODO special case where it's at the end of the file
		return -1;
	}
	int length = following_linebreak - line;
	strncpy(dst, line, length);
	*(dst + length) = '\0';
	return length;
}

int get_header_int_value(const char *headers, const char *header_prefix)
{
	char header_line_buffer[256];
	int found = find_line(header_line_buffer, headers, header_prefix);
	if (found <= 0)
		return found;
	int int_value = atoi(header_line_buffer + strlen(header_prefix));
	return int_value;
}

int get_header_char_value(const char *headers, const char *header_prefix, char *destination, size_t destination_size)
{
	char header_line_buffer[256];
	int found = find_line(header_line_buffer, headers, header_prefix);
	if (found <= 0)
		return found;
	strcpy(destination, header_line_buffer + strlen(header_prefix));
	return strlen(destination);
}

void get_file_name(const char *headers, char *destination)
{
	char filename_prefix[] = "Content-Disposition: form-data; name=\"file\"; filename=\"";
	get_header_char_value(headers, filename_prefix, destination, 256);
	//get rid of the trailing quote
	destination[strlen(destination) - 1] = '\0';
}

void get_boundary(const char *headers, char *boundary_regular, char *boundary_final)
{
	const char boundary_marker[] = "Content-Type: multipart/form-data; boundary=";
	strcpy(boundary_regular, "--"); //initialize with the boundary prefix
	get_header_char_value(headers, boundary_marker, boundary_regular + 2, 125);
	strcpy(boundary_final, boundary_regular);
	strcat(boundary_final, "--"); //last boundary ends with two extra dashes
}

void dump_request(char *buffer)
{
	debug_print("opening request.bin file\n");
	FILE *request = fopen("request.bin", "wb");
	if (request == NULL)
		return;
	debug_print("attempting to write data\n");
	fwrite(buffer, 1, strlen(buffer), request);
	fclose(request);
}

void send_post_response(){
	send(csock, http_201, strlen(http_201), 0);
    //\r\nLocation: /
	char headers_rest[] = "Content-Type: text/plain\r\nContent-Length:0\r\nConnection: Close\r\n";
	send(csock, headers_rest, strlen(headers_rest), 0);
	debug_print("Response written..\n");
}

void handle_post(char* buffer, int ret)
{
	char boundary_regular[128];
	char boundary_final[130];
	//find the multipart boundary

	const int content_length = get_header_int_value(buffer, "Content-Length:");
	debug_print("Content length:%d\n", content_length);

	int content_bytes_read = 0;

	char filename[256];
	get_file_name(buffer, filename);
	debug_print("Filename:%s", filename);
	char destination_filename[256] = "";
	strcat(destination_filename, filename);
	FILE *outfile = fopen(destination_filename, "wb");
	if (outfile == NULL)
	{
		printf("Couldn't create the output file %s!\n", destination_filename);
		return;
	}

	//check where the content starts:
	get_boundary(buffer, boundary_regular, boundary_final);
	debug_print("Regular boundary: \n%s\n", boundary_regular);
	debug_print("Final boundary: \n%s\n", boundary_final);

	char *content_start = strstr(buffer, boundary_regular);
	content_start -= 2; // CRLF
	int content_start_offset = content_start - buffer;
	debug_print("Found content start at offset %d\n", content_start_offset);
	content_bytes_read += ret - content_start_offset;

	static char file_start_marker[] = "Content-Disposition: form-data; name=\"file\";";
	//need to find two newlines from there
	char *file_start = strstr(buffer, file_start_marker);
	//advance three newlines
	for (int i = 0; i < 3; i++)
	{
		file_start = strchr(file_start, '\n') + 1;
	}

	//write the first part of the multipart we have
	fwrite(file_start, 1, ret - (file_start - buffer), outfile);
	debug_print("content_bytes_read: %d\n", content_bytes_read);
	while (content_length - content_bytes_read > 0)
	{
		int bytes_remaining = content_length - content_bytes_read;
		bool last_chunk = bytes_remaining <= DEFAULT_READ_SIZE;		
		// int bytes_to_read = bytes_remaining >= DEFAULT_READ_SIZE ? DEFAULT_READ_SIZE : bytes_remaining;
		int bytes_to_read = last_chunk ? bytes_remaining :  DEFAULT_READ_SIZE;
		ret = recv(csock, buffer, bytes_to_read, 0);
		content_bytes_read += ret;
		debug_print("Left:%d read total:%d read:%d\n", bytes_remaining, content_bytes_read, ret);
		debug_print("Last chunk? %d\n",last_chunk);
		if (ret == 0)
			break;
        //find the last multipart marker in the last chunk to know when to stop copying content
        int bytes_to_write = ret;
        if(last_chunk){
		    char *boundary_location = memmem(buffer, ret, boundary_final, strlen(boundary_final));
    		debug_print("multipart boundary @:%d\n", boundary_location-buffer);
		//subtract 2 due to previous CRLF
            bytes_to_write = boundary_location == 0 ? ret : (boundary_location - buffer - 2);
        }
		size_t written = fwrite(buffer, 1, bytes_to_write, outfile);
	}
	fclose(outfile);

	printf("Upload done\n");

	send_post_response();
}

//---------------------------------------------------------------------------------
int main(int argc, char **argv)
{
	//---------------------------------------------------------------------------------
	int ret;

	u32 clientlen;
	struct sockaddr_in client;
	struct sockaddr_in server;
	char buffer[1026];

	gfxInitDefault();

	// register gfxExit to be run when app quits
	// this can help simplify error handling
	atexit(gfxExit);

	consoleInit(GFX_TOP, NULL);

	printf("\n3DSX HTTP Uploader\n");

	// allocate buffer for SOC service
	SOC_buffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);

	if (SOC_buffer == NULL)
	{
		failExit("memalign: failed to allocate\n");
	}

	// Now intialise soc:u service
	if ((ret = socInit(SOC_buffer, SOC_BUFFERSIZE)) != 0)
	{
		failExit("socInit: 0x%08X\n", (unsigned int)ret);
	}

	// register socShutdown to run at exit
	// atexit functions execute in reverse order so this runs before gfxExit
	atexit(socShutdown);

	// libctru provides BSD sockets so most code from here is standard
	clientlen = sizeof(client);

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

	if (sock < 0)
	{
		failExit("socket: %d %s\n", errno, strerror(errno));
	}

	memset(&server, 0, sizeof(server));
	memset(&client, 0, sizeof(client));

	server.sin_family = AF_INET;
	server.sin_port = htons(80);
	server.sin_addr.s_addr = gethostid();

	printf("Point your browser to http://%s/\n", inet_ntoa(server.sin_addr));

	if ((ret = bind(sock, (struct sockaddr *)&server, sizeof(server))))
	{
		close(sock);
		failExit("bind: %d %s\n", errno, strerror(errno));
	}

	// Set socket non blocking so we can still read input to exit
	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);

	if ((ret = listen(sock, 5)))
	{
		failExit("listen: %d %s\n", errno, strerror(errno));
	}

	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		csock = accept(sock, (struct sockaddr *)&client, &clientlen);

		if (csock < 0)
		{
			if (errno != EAGAIN)
			{
				failExit("accept: %d %s\n", errno, strerror(errno));
			}
		}
		else
		{
			// set client socket to blocking to simplify sending data back
			fcntl(csock, F_SETFL, fcntl(csock, F_GETFL, 0) & ~O_NONBLOCK);
			//printf("Connecting port %d from %s\n", client.sin_port, inet_ntoa(client.sin_addr));
			memset(buffer, 0, 1026);
			ret = recv(csock, buffer, DEFAULT_READ_SIZE, 0);
			//GET handler
			if (!strncmp(buffer, http_get_index, strlen(http_get_index)))
			{

				send(csock, http_200, strlen(http_200), 0);
				send(csock, http_html_hdr, strlen(http_html_hdr), 0);
				sprintf(buffer, indexdata);
				send(csock, buffer, strlen(buffer), 0);
			}
			//POST handler
			if (!strncmp(buffer, http_post_index, strlen(http_post_index)))
			{
				handle_post(buffer, ret);
			}

			close(csock);
			csock = -1;
		}

		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break;
	}

	close(sock);

	return 0;
}

//---------------------------------------------------------------------------------
void failExit(const char *fmt, ...)
{
	//---------------------------------------------------------------------------------

	if (sock > 0)
		close(sock);
	if (csock > 0)
		close(csock);

	va_list ap;

	printf(CONSOLE_RED);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf(CONSOLE_RESET);
	printf("\nPress B to exit\n");

	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		u32 kDown = hidKeysDown();
		if (kDown & KEY_B)
			exit(0);
	}
}