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

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

static u32 *SOC_buffer = NULL;
s32 sock = -1, csock = -1;

__attribute__((format(printf,1,2)))
void failExit(const char *fmt, ...);

const static char http_200[] = "HTTP/1.1 200 OK\r\n";

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


//---------------------------------------------------------------------------------
void socShutdown() {
//---------------------------------------------------------------------------------
	printf("waiting for socExit...\n");
	socExit();

}

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
	int ret;

	u32	clientlen;
	struct sockaddr_in client;
	struct sockaddr_in server;
	char buffer[1026];

	gfxInitDefault();

	// register gfxExit to be run when app quits
	// this can help simplify error handling
	atexit(gfxExit);

	consoleInit(GFX_TOP, NULL);

	printf ("\n3DSX HTTP Uploader\n");

	// allocate buffer for SOC service
	SOC_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);

	if(SOC_buffer == NULL) {
		failExit("memalign: failed to allocate\n");
	}

	// Now intialise soc:u service
	if ((ret = socInit(SOC_buffer, SOC_BUFFERSIZE)) != 0) {
    	failExit("socInit: 0x%08X\n", (unsigned int)ret);
	}

	// register socShutdown to run at exit
	// atexit functions execute in reverse order so this runs before gfxExit
	atexit(socShutdown);

	// libctru provides BSD sockets so most code from here is standard
	clientlen = sizeof(client);

	sock = socket (AF_INET, SOCK_STREAM, IPPROTO_IP);

	if (sock < 0) {
		failExit("socket: %d %s\n", errno, strerror(errno));
	}

	memset (&server, 0, sizeof (server));
	memset (&client, 0, sizeof (client));

	server.sin_family = AF_INET;
	server.sin_port = htons (80);
	server.sin_addr.s_addr = gethostid();

	printf("Point your browser to http://%s/\n",inet_ntoa(server.sin_addr));
		
	if ( (ret = bind (sock, (struct sockaddr *) &server, sizeof (server))) ) {
		close(sock);
		failExit("bind: %d %s\n", errno, strerror(errno));
	}

	// Set socket non blocking so we can still read input to exit
	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);

	if ( (ret = listen( sock, 5)) ) {
		failExit("listen: %d %s\n", errno, strerror(errno));
	}

	while (aptMainLoop()) {
		gspWaitForVBlank();
		hidScanInput();

		csock = accept (sock, (struct sockaddr *) &client, &clientlen);

		if (csock<0) {
			if(errno != EAGAIN) {
				failExit("accept: %d %s\n", errno, strerror(errno));
			}
		} else {
			// set client socket to blocking to simplify sending data back
			fcntl(csock, F_SETFL, fcntl(csock, F_GETFL, 0) & ~O_NONBLOCK);
			printf("Connecting port %d from %s\n", client.sin_port, inet_ntoa(client.sin_addr));
			memset (buffer, 0, 1026);

			ret = recv (csock, buffer, 1024, 0);
			//GET handler
			if ( !strncmp( buffer, http_get_index, strlen(http_get_index) ) ) {

				send(csock, http_200, strlen(http_200),0);
				send(csock, http_html_hdr, strlen(http_html_hdr),0);
				sprintf(buffer, indexdata);
				send(csock, buffer, strlen(buffer),0);
			}
			//POST handler
			if(!strncmp(buffer, http_post_index, strlen(http_post_index)))
			{
				printf("checked for POST\n");
				printf("opening output file\n");
				FILE *request = fopen("output.bin","wb");
				if (request == NULL)
					continue;
				printf("attempting to write\n");
				fwrite(buffer, 1, strlen(buffer), request);
				fclose(request);
				//find the multipart boundary
				const char boundary_marker[] = "Content-Type: multipart/form-data; boundary=";
				char boundary[128];
				char* boundary_tmp = strstr(buffer, boundary_marker) + strlen(boundary_marker);
				char* nextline_after_boundary = strchr(boundary_tmp, '\n');
				strncpy(boundary, boundary_tmp, nextline_after_boundary-boundary_tmp);
				printf("Multipart boundary: %s", boundary);

				//look for line "filename=...."
				//TODO read the name later, assume upload.3dsx
				char name[] = "upload.3dsx";
				char filebuf[8000];			
				printf("attempting to create output file...\n");
				FILE *outfile = fopen("3ds/upload.3dsx","wb");
				if(outfile == NULL)
				{
					printf("Couldn't create output file!\n");
					continue;
				}

				//try to read the file into end
				//strcpy(filebuf, buffer);

				//look for the file start:
				//Content-Disposition: form-data; name="file";
				static char file_start_marker[] = "Content-Disposition: form-data; name=\"file\";";
				//need to find two newlines from there
				char* file_start = strstr(buffer, file_start_marker);
				//advance three newlines
				for(int i = 0; i < 3; i++) {
					file_start = strchr(file_start, '\n')+1;
				}

				//write the first part of the multipart we have				
				fwrite(file_start, 1, ret-(file_start-buffer), outfile);

				//read and write more segments
				// do{
				while(true){
					ret = recv (csock, buffer, 1024, 0);
					if(ret == 0)
						break;
					printf("read %d more bytes\n", ret);
					fwrite(buffer, 1, ret, outfile);
					fflush(outfile); //TODO remove
				}
				// }
				// while(ret > 0);
				fclose(outfile);

				printf("Reading complete\n");
				send(csock, http_200, strlen(http_200),0);		
			}

			printf("Closing the socket..\n");
			close (csock);
			csock = -1;
		}

		u32 kDown = hidKeysDown();
		if (kDown & KEY_START) break;
	}

	close(sock);

	return 0;
}

//---------------------------------------------------------------------------------
void failExit(const char *fmt, ...) {
//---------------------------------------------------------------------------------

	if(sock>0) close(sock);
	if(csock>0) close(csock);

	va_list ap;

	printf(CONSOLE_RED);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf(CONSOLE_RESET);
	printf("\nPress B to exit\n");

	while (aptMainLoop()) {
		gspWaitForVBlank();
		hidScanInput();

		u32 kDown = hidKeysDown();
		if (kDown & KEY_B) exit(0);
	}
}
