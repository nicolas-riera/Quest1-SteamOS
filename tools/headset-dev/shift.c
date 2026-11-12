// shift a.ppm b.ppm x0 y0 w h R: best (dx, dy) moving a's region onto b (mean abs diff of grey), on the headset only
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static unsigned char *load(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f || fscanf(f, "P6 %d %d 255", w, h) != 2)
		return NULL;
	fgetc(f);
	size_t n = (size_t)*w * *h;
	unsigned char *rgb = malloc(n * 3), *g = malloc(n);
	if (fread(rgb, 3, n, f) != n)
		return NULL;
	fclose(f);
	for (size_t i = 0; i < n; i++)
		g[i] = (unsigned char)((rgb[3 * i] + 2 * rgb[3 * i + 1] + rgb[3 * i + 2]) / 4);
	free(rgb);
	return g;
}

int main(int argc, char **argv)
{
	int w, h, w2, h2;
	unsigned char *a = load(argv[1], &w, &h), *b = load(argv[2], &w2, &h2);
	if (!a || !b || w != w2 || h != h2)
		return 1;
	int x0 = atoi(argv[3]), y0 = atoi(argv[4]), rw = atoi(argv[5]), rh = atoi(argv[6]), R = atoi(argv[7]);
	double mean = 0, var = 0;
	for (int y = y0; y < y0 + rh; y++)
		for (int x = x0; x < x0 + rw; x++)
			mean += a[y * w + x];
	mean /= (double)rw * rh;
	for (int y = y0; y < y0 + rh; y++)
		for (int x = x0; x < x0 + rw; x++)
			var += (a[y * w + x] - mean) * (a[y * w + x] - mean);
	double best = 1e18, at0 = 0;
	int bx = 0, by = 0;
	for (int pass = 0; pass < 2; pass++) {
		int step = pass ? 1 : 3, cx = bx, cy = by, r = pass ? 3 : R;
		for (int dy = cy - r; dy <= cy + r; dy += step)
			for (int dx = cx - r; dx <= cx + r; dx += step) {
				double s = 0;
				for (int y = y0; y < y0 + rh; y += 3)
					for (int x = x0; x < x0 + rw; x += 3)
						s += abs(a[y * w + x] - b[(y + dy) * w + x + dx]);
				if (dx == 0 && dy == 0)
					at0 = s;
				if (s < best)
					best = s, bx = dx, by = dy;
			}
	}
	double n = (double)(rw / 3) * (rh / 3);
	printf("size %dx%d region mean %.1f sd %.1f: shift dx %d dy %d (diff %.2f, at 0 %.2f)\n", w, h, mean,
	       sqrt(var / ((double)rw * rh)), bx, by, best / n, at0 / n);
	return 0;
}
