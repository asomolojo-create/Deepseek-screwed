#ifndef ANIMATE_H
#define ANIMATE_H

#include <stdbool.h>   /* for bool           */
#include <stdint.h>    /* for uint32_t       */
#include <sys/types.h> /* for size_t/ssize_t */

/**
 * Abstract data types
 * Exposing the content of these structs allows users to
 * make assumptions on underlying implementation details
 * If we improve the implementation, many users may need
 * to modify their code as the assumptions no longer hold.
 */
struct sprite;
struct canvas;
struct sprite_placement;

/**
 * @brief Representation of a pixel
 */
typedef uint32_t color_t;

/**
 * @brief Custom animation function for optional extension.
 * @details
 * @param priv a handle to the private data provided when the function was
 *        registered
 * @param x a reference to the starting position. On return, this should
 *        contain the target sprite position for the frame in question.
 * @param y a reference to the starting position. On return, this should
 *        contain the position for the frame in question.
 * @param t the current time since the start of the animation.
 */
typedef void (*animate_fn)(void* priv, ssize_t* x, ssize_t* y, float t);

// Inline functions are often preferred over macros as types will be checked
/**
 * @brief Helper function for creating assigning an rgb value to a pixel
 */
static inline color_t animate_color_rgb(unsigned r, unsigned g, unsigned b) {
    return ((r & 0xff) << 16) | ((g & 0xff) << 8) | ((b & 0xff) << 0);
}

/**
 * @brief Helper function for creating assigning an argb value to a pixel
 */
static inline color_t animate_color_argb(unsigned a,
                                         unsigned r, unsigned g, unsigned b) {
    return animate_color_rgb(r, g, b) | ((a & 0xff) << 24);
}

/**
 * @brief Create the canvas on which our animation will be built.
 * @details
 * @param height height of the drawing area in pixels
 * @param width width of the drawing area in pixels
 * @param background_color the colour to use when no sprites overlap a pixel.
 * @return Abstract data type for the canvas. The memory associated this struct
 * is allocated by this function and cleaned up by the corresponding destroy
 * function
 */
struct canvas* animate_create_canvas(size_t height, size_t width,
                                     color_t background_color);

/**
 * @brief Clean up a canvas
 * @details
 * Frees all memory associated with the canvas, including placements, but not
 * sprites
 * @param canvas the canvas to clean up
 */
void animate_destroy_canvas(struct canvas* canvas);

/**
 * @brief Create a sprite from the provided input bitmap file
 * @details
 * The file format can be assumed to be ARGB32. The bitmap content may
 * by loaded only on first access, however it must only be loaded once.
 * NOTE: the order of image rows differs to what we expect. The first
 * pixel in the bmp file format is on the bottom left while the last
 * pixel is the top right. For simplicity, reverse the order of rows
 * while loading the bitmap.
 * @param file the name of the bitmap file to be loaded
 */
struct sprite* animate_create_sprite(const char* file);

/**
 * @brief Create a sprite that represents a rectangle
 *
 * @details
 * @param width the width of the rectangle
 * @param height the height of the rectangle
 * @param c the colour of the rectangle
 * @param filled true if the rectangle should be filled, otherwise false
 * @return A handle to the created sprite
 */
struct sprite* animate_create_rectangle(size_t width, size_t height, color_t c,
                                        bool filled);

/**
 * @brief Create a sprite that represents a circle
 *
 * @details
 * We currently only support filled circles. The circle pixels can be
 * determined by the following formula:
 *   x*x + y*y < r*r
 * Where x is the x offset from the center of the circle
 *       y is the y offset from the center of the circle
 *       r is the radius of the circle
 *
 * @param radius the radius of the circle
 * @param c the colour of the circle
 * @param filled reserved for future use
 * @return A handle to the created sprite
 */
struct sprite* animate_create_circle(size_t radius, color_t c, bool filled);

/**
 * @brief Frees all memory associated with a sprite
 * @details
 * @param sprite the sprite to clean up
 * @return 0 on success, otherwise an error occurred. The only error
 * possible is that the sprite is still in use
 */
bool animate_destroy_sprite(struct sprite* sprite);

/**
 * @brief Places a sprite on the top layer of the canvas
 * @details
 * @param canvas the canvas to place the sprite on
 * @param sprite the sprite to place
 * @param x the x coordinate of the canvas to which the top left pixel of the
 *          sprite should be placed
 * @param y the y coordinate of the canvas to which the top left pixel of the
 *          sprite should be placed
 * @return A handle to a sprite placement.
 */
struct sprite_placement* animate_place_sprite(struct canvas* canvas, struct sprite* sprite, ssize_t x, ssize_t y);

/**
 * @brief Raise a sprite one level towards the top layer
 * @details
 * @param sprite_placement a handle to the placement
 */
void animate_placement_up(struct sprite_placement* sprite_placement);

/**
 * @brief Lower a sprite one level towards the bottom layer
 * @details
 * @param sprite_placement a handle to the placement
 */
void animate_placement_down(struct sprite_placement* sprite_placement);

/**
 * @brief Raise the sprite to the top-most layer
 * @details
 * @param sprite_placement a handle to the placement
 */
void animate_placement_top(struct sprite_placement* sprite_placement);

/**
 * @brief Lower the sprite to the bottom-most layer
 * @details
 * @param sprite_placement a handle to the placement
 */
void animate_placement_bottom(struct sprite_placement* sprite_placement);

/**
 * @brief Removes the placement from its associated canvas and free resources
 * @details
 * @param sprite_placement a handle to the placement
 */
void animate_destroy_placement(struct sprite_placement* sprite_placement);

/* Accessor to get the canvas associated with a placement (safe opaque access) */
struct canvas* animate_get_placement_canvas(struct sprite_placement* p);


/**
 * @brief Configure sprite animation
 * @details
 * Currently, we support only physics simulation (velocity and acceleration)
 * @param sprite_placement a handle to the sprite placement
 * @param vx the initial velocity in the x direction
 * @param vy the initial velocity in the y direction
 * @param ax the acceleration in the x direction
 * @param ay the acceleration in the y direction
 */
void animate_set_animation_params(struct sprite_placement* sprite_placement, ssize_t vx, ssize_t vy, ssize_t ax, ssize_t ay);

/**
 * @brief [Optional extension] define animation using the provided function
 *                             pointer
 * @param sprite_placement a handle to the sprite placement
 * @param animate_fn a custom function to call when determining the position of
 *                   the sprite in each frame.
 * @param priv       private data that is passed to the provided function on
 *                   call.
 */
void animate_set_animation_function(struct sprite_placement* sprite_placement,
                                    animate_fn fn, void* priv);

/**
 * @brief Return the size in bytes of an animation frame
 * @details
 * This is useful for the caller to determine how much memory to allocate when
 * combining sprites into a frame.
 * @param canvas the canvas in question
 * @return the size, in bytes, of a frame for this canvas
 */
size_t animate_frame_size_bytes(struct canvas* canvas);

/**
 * Accessors for sprite dimensions (do not access struct internals directly)
 */
size_t animate_sprite_width(const struct sprite* s);
size_t animate_sprite_height(const struct sprite* s);

/**
 * @brief Generate a frame of the animation
 * @details
 * @param canvas the canvas describing the animation
 * @param frame the frame number (starting from 0)
 * @param frame_rate the number of frames per second in the animation
 * @param buf a buffer provided by the user that is large enough to hold a frame.
 * This buffer will contain the frame data on return.
 */
void animate_generate_frame(const struct canvas* canvas,
                            size_t frame, size_t frame_rate,
                            void* buf);

// Accessor functions for generate command
size_t animate_get_canvas_width(struct canvas* canvas);
size_t animate_get_canvas_height(struct canvas* canvas);

#endif /* ANIMATE_H */
