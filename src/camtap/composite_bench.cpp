// Times ptprovider's composite (RenderComposite) on synthetic images with the real calibration: numbers only.
// Build: see build_probe.sh. Run on the headset: composite_bench [frames=300]   (taskset to pick a core)
#define main ptprovider_main
#include "ptprovider.cpp"
#undef main

int main(int argc, char **argv)
{
	const int frames = argc > 1 ? atoi(argv[1]) : 300;
	const Calib bottomCal[2] = {LoadCalib(0), LoadCalib(2)}, topCal[2] = {LoadCalib(1), LoadCalib(3)};
	if (!bottomCal[0].ok || !topCal[0].ok) {
		fprintf(stderr, "no calibration\n");
		return 1;
	}
	uint32_t grayPx[256];
	for (int v = 0; v < 256; v++)
		grayPx[v] = 0xff000000u | 128u << 16 | (uint32_t)v << 8 | 128u;
	std::vector<uint8_t> bottom((size_t)kW * kH), top((size_t)kW * kH);
	for (size_t i = 0; i < bottom.size(); i++)
		bottom[i] = (uint8_t)(i * 7 + (i >> 9)), top[i] = (uint8_t)(i * 13 + (i >> 8));
	std::vector<uint32_t> out((size_t)kW * kH);
	for (int c = 0; c < 2; c++) {
		const Calib v = VirtualCamera(bottomCal[c]);
		const Calib pair[2] = {bottomCal[c], topCal[c]};
		const Composite cp = BuildComposite(v, pair);
		double t0 = NowNs();
		double sumB, sumT;
		int n;
		for (int f = 0; f < frames; f++)
			RenderComposite(cp, bottom.data(), top.data(), 256, grayPx, out.data(), sumB, sumT, n);
		double ms = (NowNs() - t0) / 1e6 / frames;
		printf("eye %d: %.2f ms per image (f %.1f; %zu px from the bottom camera, %zu from the top, %zu blended, %zu empty)\n",
		       c, ms, v.f, cp.bottom.size(), cp.top.size(), cp.both.size(), cp.empty);
	}
	return 0;
}
