// Times ptprovider's composite (RenderComposite) on synthetic images with the real calibration: numbers only.
// Build: see build_probe.sh. Run on the headset: composite_bench [frames=300]   (taskset to pick a core)
// The timings are noisy while the headset runs anything else (the loop waits on DRAM): minimum and median are printed.
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
	uint8_t toSrgb[256];
	for (int v = 0; v < 256; v++)
		toSrgb[v] = (uint8_t)v;
	std::vector<uint8_t> bottom((size_t)kW * kH), top((size_t)kW * kH);
	for (size_t i = 0; i < bottom.size(); i++)
		bottom[i] = (uint8_t)(i * 7 + (i >> 9)), top[i] = (uint8_t)(i * 13 + (i >> 8));
	std::vector<uint32_t> out((size_t)kW * kH);
	for (int c = 0; c < 2; c++) {
		const Calib v = VirtualCamera(bottomCal[c]);
		const Calib pair[2] = {bottomCal[c], topCal[c]};
		const Composite cp = BuildComposite(v, pair);
		std::vector<double> ms;
		double sumB, sumT;
		int n;
		for (int f = 0; f < frames; f++) {
			double t0 = NowNs();
			RenderComposite(cp, bottom.data(), top.data(), 256, toSrgb, out.data(), sumB, sumT, n);
			ms.push_back((NowNs() - t0) / 1e6);
		}
		std::sort(ms.begin(), ms.end());
		printf("eye %d: %.2f ms per image, best %.2f (f %.1f; %zu px from the bottom camera, %zu from the top, %zu blended, "
		       "%zu empty; %zu runs)\n",
		       c, ms[ms.size() / 2], ms[0], v.f, cp.bottom.size(), cp.top.size(), cp.wTop.size(), cp.empty, cp.runs.size());
	}
	return 0;
}
