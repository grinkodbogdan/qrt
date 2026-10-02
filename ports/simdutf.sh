VERSION=9.1.0
fetch() { fetch_git https://github.com/simdutf/simdutf v$VERSION; }
build() { cmake_build . -DSIMDUTF_TESTS=OFF -DSIMDUTF_TOOLS=OFF -DSIMDUTF_BENCHMARKS=OFF; }
