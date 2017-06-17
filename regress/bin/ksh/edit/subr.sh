# Copyright (c) 2016 Ingo Schwarze <schwarze@openbsd.org>
# Copyright (c) 2017 Anton Lindqvist <anton@openbsd.org>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

testseq() {
	stdin=$1
	exp=$(echo "$2")
	act=$(echo -n "$stdin" | ./edit ksh -r)
	[ "$exp" = "$act" ] && return 0

	echo input:
	echo ">>>$stdin<<<"
	echo -n "$stdin" | hexdump -C
	echo expected:
	echo ">>>$exp<<<"
	echo -n "$exp" | hexdump -C
	echo actual:
	echo ">>>$act<<<"
	echo -n "$act" | hexdump -C

	return 1
}
