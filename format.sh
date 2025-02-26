#!/bin/bash

# for now c++ sources only

FORMAT_BINARY=${FORMAT_BINARY:-"clang-format-18"}
SOURCE_DIRS=${CHECK_PATHS:-"src src/fmt src/util src/api/cpp"}
CMD=${1:-""}

if [ "$CMD" != "format" ] && [ "$CMD" != "check" ]; then
	echo -e "Usage:\n\
    \t$0 [help] - show this message\n\
    \t$0 format - format *.cpp and *.hpp sources at SOURCE_DIRS using FORMAT_BINARY\n\
    \t$0 check  - check that formatting is necessary"
    exit 0
fi

cpp_sources=""
for dir in $SOURCE_DIRS; do
    if [ ! -d $dir ]; then
        echo "Cannot find directory" $dir >&2
        exit 2
    fi
    cpp_sources+=" "$(find $dir -maxdepth 1 -name "*.cpp" -or -name "*.hpp")
done

format_flags=""
if [ $CMD == "check" ]; then
    format_flags+="--dry-run --Werror"
else
    format_flags+="-i"
fi

exit_code="0"
for file in $cpp_sources; do
    $FORMAT_BINARY $format_flags $file
    if [ ! $? -eq 0 ]; then
        exit_code="1"
    fi
done

exit $exit_code
