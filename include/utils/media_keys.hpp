/**
 * VitaPlex - Synthetic keycodes for desktop media keys
 *
 * GLFW has no media keycodes. Play, Stop, Next and Previous all arrive at its
 * key callback as GLFW_KEY_UNKNOWN (-1) with nothing but a platform scancode to
 * tell them apart, and stock borealis discards the scancode — so by the time a
 * key reaches the app every media key looks identical.
 *
 * patches/glfw_input.cpp translates the scancodes and reports these values
 * instead. They sit above GLFW_KEY_LAST (348) so they cannot collide with a
 * real key, and they only ever appear on the desktop (GLFW) backend; the SDL
 * platforms carry media keys by their own route.
 *
 * KEEP IN SYNC with the scancode table in patches/glfw_input.cpp.
 */

#pragma once

namespace vitaplex::mediakey {

constexpr int PLAY_PAUSE = 400;
constexpr int STOP       = 401;
constexpr int NEXT       = 402;
constexpr int PREV       = 403;

}  // namespace vitaplex::mediakey
