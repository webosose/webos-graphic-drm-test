#include "readpng.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "png.h"

/*
 * To avoid this error with libpng 1.6
 * drmplanes/readpng.c:58:43: error: 'png_infopp_NULL' undeclared (first use in this function);
 */
#ifndef png_infopp_NULL
#define png_infopp_NULL           NULL
#endif

#ifndef int_p_NULL
#define int_p_NULL                NULL
#endif

struct png_buffer {
    png_uint_32 width;
    png_uint_32 height;
    png_uint_32 stride;
    void* addr;
};

/* TODO: set from user */
png_voidp user_error_ptr (png_structp png_struct, png_size_t size)
{
    return NULL;
}

void user_error_fn (png_structp png_struct, png_const_charp charp)
{

}

void user_warning_fn (png_structp png_struct, png_const_charp charp)
{

}

static void alpha_transform_func(png_structp png_ptr, png_row_infop row_info, png_bytep data)
{
    if (row_info->pixel_depth == 32)
    {
        int i;
        unsigned int* p = (unsigned int*)data;

        for (i = 0;i < row_info->width;i++, p++)
        {
            if (!(*p & 0xff000000)) *p = 0x00000000;    /* if alpha is zero, clear it with 0x00000000*/
        }
    }
}

bool read_png(FILE *fp, unsigned int sig_read, png_buffer_handle *out_png_buffer_handle)
{
    png_structp png_ptr;
    png_infop info_ptr;
    png_uint_32 width, height;
    int bit_depth, color_type, interlace_type;

    /* Create and initialize the png_struct with the desired error handler
     * functions.  If you want to use the default stderr and longjump method,
     * you can supply NULL for the last three parameters.  We also supply the
     * the compiler header file version, so that we know if the application
     * was compiled with a compatible version of the library.  REQUIRED
     */
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
        user_error_ptr, user_error_fn, user_warning_fn);

    if (png_ptr == NULL)
    {
        fclose(fp);
        return false;
    }

    /* Allocate/initialize the memory for image information.  REQUIRED. */
    info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL)
    {
        fclose(fp);
        png_destroy_read_struct(&png_ptr, png_infopp_NULL, png_infopp_NULL);
        return false;
    }

    /* Set error handling if you are using the setjmp/longjmp method (this is
     * the normal method of doing things with libpng).  REQUIRED unless you
     * set up your own error handlers in the png_create_read_struct() earlier.
     */

    if (setjmp(png_jmpbuf(png_ptr)))
    {
        /* Free all of the memory associated with the png_ptr and info_ptr */
        png_destroy_read_struct(&png_ptr, &info_ptr, png_infopp_NULL);
        fclose(fp);
        /* If we get here, we had a problem reading the file */
        return false;
    }

    /* One of the following I/O initialization methods is REQUIRED */
    /* Set up the input control if you are using standard C streams */
    png_init_io(png_ptr, fp);

    /* If we have already read some of the signature */
    png_set_sig_bytes(png_ptr, sig_read);

    /* OK, you're doing it the hard way, with the lower-level functions */

    /* The call to png_read_info() gives us all of the information from the
     * PNG file before the first IDAT (image data chunk).  REQUIRED
     */
    png_read_info(png_ptr, info_ptr);

    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type,
        &interlace_type, int_p_NULL, int_p_NULL);

    printf("width: %u height: %u bit_depth: %u\n",
        (uint32_t)width, (uint32_t)height, (uint32_t)bit_depth);

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);

    /* if png depth is less than 8, expand it to 8-bit*/
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }

    /* set tRNS to alpha */
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);

    png_bytep trans_alpha = NULL;
    int num_trans = 0;

    if (bit_depth == 32)
    {
        png_set_swap_alpha(png_ptr);  /* re-arrange into 0xAARRGGBB. If we do not, the order is 0xRRGGBBAA */
    }
    else if (((png_get_tRNS(png_ptr, info_ptr, &trans_alpha, &num_trans,
            NULL) & PNG_INFO_tRNS) && num_trans > 0) ||
        (color_type & PNG_COLOR_MASK_ALPHA))
    {
        /* don't change color order */
    }
    else
    {
        png_set_strip_alpha(png_ptr);
    }

    if (bit_depth == 16) png_set_strip_16(png_ptr);
    if (bit_depth < 8) png_set_packing(png_ptr);

    if (color_type == PNG_COLOR_TYPE_RGB_ALPHA)
        png_set_read_user_transform_fn(png_ptr, alpha_transform_func);

    /* convert grayscale to RGB */
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb (png_ptr);

    if (interlace_type != PNG_INTERLACE_NONE)
        png_set_interlace_handling(png_ptr);

    png_set_bgr(png_ptr);
    png_set_filler(png_ptr, 0xff, PNG_FILLER_AFTER);
    png_read_update_info(png_ptr, info_ptr);

    /* Allocate the memory to hold the image using the fields of info_ptr. */

    /* The easiest way to read the image: */
    png_bytep row_pointers[height];
    png_uint_32 row, rowbytes;

    /* Clear the pointer array */
    for (row = 0; row < height; row++)
        row_pointers[row] = NULL;

    rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    printf("rowbytes: %u\n", (uint32_t)rowbytes);

    /*
     * for (row = 0; row < height; row++)
     *     row_pointers[row] = png_malloc(png_ptr, rowbytes);
     */

    struct png_buffer *png_buffer = calloc(sizeof(struct png_buffer), 1);
    if (!png_buffer) {
        fprintf(stderr,"fail to create png_buffer\n");
        return false;
    }
    png_buffer->width = width;
    png_buffer->height = height;
    png_buffer->stride = rowbytes;
    png_buffer->addr = malloc(png_buffer->stride * height);
    if (!png_buffer->addr) {
        fprintf(stderr,"fail to create png_buffer addr\n");
        return false;
    }

    for (row = 0; row < height; row++)
        row_pointers[row] = png_buffer->addr + row * rowbytes;

    /* Now it's time to read the image.  One of these methods is REQUIRED */
    png_read_image(png_ptr, row_pointers);

    /* Read rest of file, and get additional chunks in info_ptr - REQUIRED */
    png_read_end(png_ptr, info_ptr);

    /* At this point you have read the entire image */

    *out_png_buffer_handle = png_buffer;

    /* Clean up after the read, and free any memory allocated - REQUIRED */
    png_destroy_read_struct(&png_ptr, &info_ptr, png_infopp_NULL);

    /* Close the file */
    fclose(fp);

    /* That's it */
    return true;
}

bool fill_buffer(void* addr, uint32_t width, uint32_t height, uint32_t stride, png_buffer_handle png_buffer_handle)
{
    struct png_buffer *png_buffer = png_buffer_handle;
    if (!(png_buffer->width == width && png_buffer->height == height && png_buffer->stride == stride)) {
        fprintf(stderr, "png_buffer mismatch %u = %u, %u = %u, %u = %u\n",
            (uint32_t)png_buffer->width, width,
            (uint32_t)png_buffer->height, height,
            (uint32_t)png_buffer->stride, stride);
        return false;
    }

    png_uint_32 row;

    for (row = 0; row < height; row++)
        memcpy(addr + row * stride, png_buffer->addr + row * stride, stride);

    return true;
}

void destroy_png_buffer(png_buffer_handle png_buffer_handle)
{
    struct png_buffer *png_buffer = png_buffer_handle;

    if (png_buffer) {
        free(png_buffer->addr);
        free(png_buffer);
    }
}
