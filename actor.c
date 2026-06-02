#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define SOCKET_PATH "/var/run/snake_actor.socket"
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

int fb_fd = -1;
void *fb_mem = NULL;
int screen_width = 0;
int screen_height = 0;
int bytes_per_pixel = 4;

FT_Library ft = NULL;
FT_Face face = NULL;

int server_fd = -1;

/* JPEG error handler */
struct my_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

METHODDEF(void) my_error_exit(j_common_ptr cinfo) {
    struct my_error_mgr *myerr = (struct my_error_mgr*) cinfo->err;
    longjmp(myerr->setjmp_buffer, 1);
}

/* Cleanup on exit */
void cleanup(int sig) {
    (void)sig;
    if (fb_mem && fb_mem != MAP_FAILED) {
        munmap(fb_mem, screen_width * screen_height * bytes_per_pixel);
    }
    if (fb_fd >= 0) close(fb_fd);
    if (face) FT_Done_Face(face);
    if (ft) FT_Done_FreeType(ft);
    if (server_fd >= 0) {
        close(server_fd);
        unlink(SOCKET_PATH);
    }
    exit(EXIT_SUCCESS);
}

/* Draw a single pixel */
void draw_pixel(int x, int y, unsigned int color) {
    if (x < 0 || x >= screen_width || y < 0 || y >= screen_height) return;
    unsigned int *pixel = (unsigned int*)((char*)fb_mem + (y * screen_width * bytes_per_pixel) + (x * bytes_per_pixel));
    *pixel = color;
}

/* Draw PNG */
void draw_png(int x, int y, const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("PNG: Cannot open %s\n", filename);
        return;
    }
    
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return;
    }
    
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return;
    }
    
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return;
    }
    
    png_init_io(png, fp);
    png_read_info(png, info);
    
    int width = png_get_image_width(png, info);
    int height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);
    
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY) png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    
    png_bytep *row_pointers = malloc(height * sizeof(png_bytep));
    for (int row = 0; row < height; row++) {
        row_pointers[row] = malloc(png_get_rowbytes(png, info));
    }
    png_read_image(png, row_pointers);
    
    for (int py = 0; py < height && y + py < screen_height; py++) {
        for (int px = 0; px < width && x + px < screen_width; px++) {
            png_bytep px_ptr = &row_pointers[py][px * 4];
            unsigned int color = (px_ptr[3] << 24) | (px_ptr[0] << 16) | (px_ptr[1] << 8) | px_ptr[2];
            draw_pixel(x + px, y + py, color);
        }
    }
    
    for (int row = 0; row < height; row++) free(row_pointers[row]);
    free(row_pointers);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
}

/* Draw JPEG */
void draw_jpeg(int x, int y, const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("JPEG: Cannot open %s\n", filename);
        return;
    }
    
    struct jpeg_decompress_struct cinfo;
    struct my_error_mgr jerr;
    
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;
    
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return;
    }
    
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);
    
    int width = cinfo.output_width;
    int height = cinfo.output_height;
    int row_stride = width * 3;
    
    JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)
        ((j_common_ptr) &cinfo, JPOOL_IMAGE, row_stride, 1);
    
    for (int py = 0; py < height && y + py < screen_height; py++) {
        jpeg_read_scanlines(&cinfo, buffer, 1);
        for (int px = 0; px < width && x + px < screen_width; px++) {
            JSAMPLE *sample = &buffer[0][px * 3];
            unsigned int color = 0xFF000000 | (sample[0] << 16) | (sample[1] << 8) | sample[2];
            draw_pixel(x + px, y + py, color);
        }
    }
    
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
}

/* Draw text using FreeType */
void draw_text(int x, int y, int size, const char *text, unsigned int color) {
    if (!face) {
        printf("draw_text: font not loaded\n");
        return;
    }
    FT_Set_Pixel_Sizes(face, 0, size);
    int start_x = x;
    
    for (const char *p = text; *p; p++) {
        if (FT_Load_Char(face, *p, FT_LOAD_RENDER))
            continue;
        
        for (int row = 0; row < face->glyph->bitmap.rows; row++) {
            for (int col = 0; col < face->glyph->bitmap.width; col++) {
                int px = start_x + col;
                int py = y + row;
                if (px >= screen_width || py >= screen_height) continue;
                
                unsigned char pixel = face->glyph->bitmap.buffer[row * face->glyph->bitmap.pitch + col];
                if (pixel > 128) draw_pixel(px, py, color);
            }
        }
        start_x += face->glyph->advance.x >> 6;
    }
}

/* Parse incoming commands */
void parse_command(char *cmd) {
    int x, y, size;
    unsigned int color;
    char filename[256];
    char text[256];
    
    if (sscanf(cmd, "PNG %d %d %255s", &x, &y, filename) == 3) {
        draw_png(x, y, filename);
    }
    else if (sscanf(cmd, "JPEG %d %d %255s", &x, &y, filename) == 3) {
        draw_jpeg(x, y, filename);
    }
    else if (sscanf(cmd, "TEXT %d %d %d %x %[^\n]", &x, &y, &size, &color, text) == 5) {
        draw_text(x, y, size, text, color);
    }
    else if (strcmp(cmd, "CLEAR") == 0) {
        for (int i = 0; i < screen_width * screen_height; i++) {
            ((unsigned int*)fb_mem)[i] = 0xFF000000;
        }
    }
    else if (strcmp(cmd, "TEST") == 0) {
        for (int i = 0; i < screen_width * screen_height; i++) {
            ((unsigned int*)fb_mem)[i] = 0xFFFF0000;
        }
    }
}

int main() {
    printf("Actor starting...\n");
    
    /* Signal handlers */
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);
    
    /* Open framebuffer */
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("open /dev/fb0");
        return EXIT_FAILURE;
    }
    
    /* Get framebuffer info */
    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        perror("ioctl FBIOGET_VSCREENINFO");
        close(fb_fd);
        return EXIT_FAILURE;
    }
    
    screen_width = vinfo.xres;
    screen_height = vinfo.yres;
    bytes_per_pixel = vinfo.bits_per_pixel / 8;
    
    if (bytes_per_pixel != 4) {
        fprintf(stderr, "Framebuffer has %d bpp, but Actor requires 32bpp (RGBA)\n", vinfo.bits_per_pixel);
        close(fb_fd);
        return EXIT_FAILURE;
    }
    
    printf("Framebuffer: %dx%d, %d bpp\n", screen_width, screen_height, vinfo.bits_per_pixel);
    
    /* mmap framebuffer */
    fb_mem = mmap(NULL, screen_width * screen_height * bytes_per_pixel,
                  PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("mmap");
        close(fb_fd);
        return EXIT_FAILURE;
    }
    
    /* Initialize FreeType */
    if (FT_Init_FreeType(&ft)) {
        fprintf(stderr, "Failed to init FreeType\n");
        cleanup(0);
    }
    
    if (FT_New_Face(ft, "/system/fonts/FreeSans.ttf", 0, &face)) {
        printf("Warning: font not loaded\n");
        face = NULL;
    } else {
        FT_Set_Pixel_Sizes(face, 0, 24);
    }
    
    /* Create socket */
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        cleanup(0);
    }
    
    struct sockaddr_un addr;
    unlink(SOCKET_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        cleanup(0);
    }
    
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        cleanup(0);
    }
    
    printf("Actor ready on %s\n", SOCKET_PATH);
    printf("Commands: PNG, JPEG, TEXT, TEST\n");
    
    /* Main loop */
    char buffer[1024];
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        
        memset(buffer, 0, sizeof(buffer));
        ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            buffer[strcspn(buffer, "\r\n")] = '\0';
            parse_command(buffer);
        }
        close(client_fd);
    }
    
    return EXIT_SUCCESS;
}
