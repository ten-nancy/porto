project(fmt)
cmake_minimum_required(VERSION 3.18)

add_library(fmt STATIC format.cc ostream.cc)
