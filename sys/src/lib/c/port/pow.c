/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

#include <u.h>
#include <libc.h>

double
jehanne_pow(double x, double y) /* return x ^ y (exponentiation) */
{
	double xy, y1, ye;
	int32_t i;
	int ex, ey, flip;

	if(y == 0.0)
		return 1.0;

	flip = 0;
	if(y < 0.){
		y = -y;
		flip = 1;
	}
	y1 = jehanne_modf(y, &ye);
	if(y1 != 0.0){
		if(x <= 0.)
			goto zreturn;
		if(y1 > 0.5) {
			y1 -= 1.;
			ye += 1.;
		}
		xy = jehanne_exp(y1 * jehanne_log(x));
	}else
		xy = 1.0;
	if(ye > 0x7FFFFFFF){	/* should be ~0UL but compiler can't convert double to ulong */
		if(x <= 0.){
 zreturn:
			if(x==0. && !flip)
				return 0.;
			return jehanne_NaN();
		}
		if(flip){
			if(y == .5)
				return 1./jehanne_sqrt(x);
			y = -y;
		}else if(y == .5)
			return jehanne_sqrt(x);
		return jehanne_exp(y * jehanne_log(x));
	}
	x = jehanne_frexp(x, &ex);
	ey = 0;
	i = ye;
	if(i)
		for(;;){
			if(i & 1){
				xy *= x;
				ey += ex;
			}
			i >>= 1;
			if(i == 0)
				break;
			x *= x;
			ex <<= 1;
			if(x < .5){
				x += x;
				ex -= 1;
			}
		}
	if(flip){
		xy = 1. / xy;
		ey = -ey;
	}
	return jehanne_ldexp(xy, ey);
}
