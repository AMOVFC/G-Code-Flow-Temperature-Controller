# Warning configuration.
#
# Applied via the sb53_warnings interface target. The core library treats warnings as
# errors; frontends are held to the same standard but without -Werror, since Qt headers
# generate warnings we do not control.

add_library(sb53_warnings INTERFACE)

if(MSVC)
    target_compile_options(sb53_warnings INTERFACE
        /W4
        /permissive-        # standards conformance
        /w14242             # narrowing conversion
        /w14254             # narrowing in a bitfield
        /w14263             # member function hides a virtual
        /w14265             # non-virtual destructor on a polymorphic class
        /w14287             # unsigned/negative constant mismatch
        /w14296             # expression always true/false
        /w14545             # malformed expression before comma
        /w14619             # unknown #pragma warning
        /w14640             # non-thread-safe static initialisation
        /w14826             # sign-extending conversion
        /w14928             # illegal copy-initialisation
    )
else()
    target_compile_options(sb53_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion        # see note below
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wnull-dereference
    )
endif()

# -Wconversion is deliberately on.
#
# This code mixes floating-point flow/temperature values with integer indices and
# counters constantly. Silent narrowing is one of the easier ways to introduce a subtle
# numerical bug here, and such a bug would surface as a bad print rather than a crash.
# Where a conversion is genuinely intended, write it explicitly.

add_library(sb53_werror INTERFACE)
if(MSVC)
    target_compile_options(sb53_werror INTERFACE /WX)
else()
    target_compile_options(sb53_werror INTERFACE -Werror)
endif()
