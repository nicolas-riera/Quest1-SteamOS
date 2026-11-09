// The Quest 1's GPU (Adreno 540, kgsl) in KDE's System Monitor: a ksystemstats plugin with the stock GPU plugin's
// id and sensor names (gpu/all/usage, gpu/gpu0/usage, coreFrequency, temperature, usedVram...), so System
// Monitor's GPU views find it. The stock plugin only knows AMD, Intel and NVIDIA through DRM, and there is no DRM
// device here (kgsl, through libhybris), so its GPU tile stayed empty.
//
// Installed as /usr/local/lib/qt6/plugins/ksystemstats/ksystemstats_plugin_gpu.so; ksystemstats gets
// QT_PLUGIN_PATH=/usr/local/lib/qt6/plugins (a drop-in of plasma-ksystemstats.service, device/holo-desktop.sh):
// KDE loads the first plugin of an id it finds, so this one replaces the stock plugin without touching KDE's files.
//
// From /sys/class/kgsl: kgsl-3d0/gpu_busy_percentage ("82 %"), clock_mhz, temp (tenths of a degree),
// gpu_model; kgsl/page_alloc (bytes of GPU memory allocated, in system RAM: the memory is shared, so the total
// is the RAM's).
#include <KPluginFactory>
#include <QFile>
#include <QRegularExpression>
#include <systemstats/AggregateSensor.h>
#include <systemstats/SensorContainer.h>
#include <systemstats/SensorObject.h>
#include <systemstats/SensorPlugin.h>
#include <systemstats/SensorProperty.h>
#include <unistd.h>

using namespace KSysGuard;

static QByteArray read_sysfs(const char *path)
{
	QFile f(QString::fromLatin1(path));
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return f.readAll().trimmed();
}

// the number a sysfs file starts with ("82 %" -> 82)
static qlonglong read_num(const char *path)
{
	QByteArray s = read_sysfs(path);
	int n = 0;
	while (n < s.size() && (isdigit((unsigned char)s[n]) || (n == 0 && s[n] == '-')))
		n++;
	return s.left(n).toLongLong();
}

class Gpu : public SensorObject
{
	Q_OBJECT
public:
	explicit Gpu(SensorContainer *parent)
		: SensorObject(QStringLiteral("gpu0"), QStringLiteral("GPU 1"), parent)
	{
		QString model = QString::fromLatin1(read_sysfs("/sys/class/kgsl/kgsl-3d0/gpu_model"));
		model.replace(QRegularExpression(QStringLiteral("^Adreno(\\d+).*")), QStringLiteral("Adreno \\1"));
		if (model.isEmpty())
			model = QStringLiteral("Adreno");
		auto prop = [this](const char *id, const QString &title, KSysGuard::Unit unit) {
			auto p = new SensorProperty(QString::fromLatin1(id), QString::fromLatin1(id), 0, this);
			p->setName(title);
			p->setPrefix(name());
			p->setUnit(unit);
			return p;
		};
		m_name = new SensorProperty(QStringLiteral("name"), this);
		m_name->setName(QStringLiteral("Name"));
		m_name->setPrefix(name());
		m_name->setValue(model);
		m_usage = prop("usage", QStringLiteral("Usage"), UnitPercent);
		m_usage->setMin(0);
		m_usage->setMax(100);
		m_total = prop("totalVram", QStringLiteral("Total Video Memory"), UnitByte);
		m_total->setShortName(QStringLiteral("Total"));
		m_used = prop("usedVram", QStringLiteral("Video Memory Used"), UnitByte);
		m_used->setShortName(QStringLiteral("Used"));
		m_used->setMax(m_total);
		m_freq = prop("coreFrequency", QStringLiteral("Frequency"), UnitMegaHertz);
		m_freq->setMax(read_num("/sys/class/kgsl/kgsl-3d0/max_clock_mhz"));
		m_temp = prop("temperature", QStringLiteral("Temperature"), UnitCelsius);
		m_total->setValue((qlonglong)sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE)); // shared with the CPU
	}

	void update()
	{
		if (m_usage->isSubscribed())
			m_usage->setValue(double(read_num("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage")));
		if (m_freq->isSubscribed())
			m_freq->setValue(double(read_num("/sys/class/kgsl/kgsl-3d0/clock_mhz")));
		if (m_temp->isSubscribed())
			m_temp->setValue(read_num("/sys/class/kgsl/kgsl-3d0/temp") / 10.0);
		if (m_used->isSubscribed())
			m_used->setValue(read_num("/sys/class/kgsl/kgsl/page_alloc"));
	}

private:
	SensorProperty *m_name, *m_usage, *m_total, *m_used, *m_freq, *m_temp;
};

// "All GPUs", as the stock plugin has it (System Monitor's overview reads gpu/all/usage)
class AllGpus : public SensorObject
{
	Q_OBJECT
public:
	explicit AllGpus(SensorContainer *parent)
		: SensorObject(QStringLiteral("all"), QStringLiteral("All GPUs"), parent)
	{
		const QRegularExpression others(QStringLiteral("^(?!all).*$"));
		auto usage = new AggregateSensor(this, QStringLiteral("usage"), QStringLiteral("All GPUs Usage"), 0);
		usage->setShortName(QStringLiteral("Usage"));
		usage->setMatchSensors(others, QStringLiteral("usage"));
		usage->setUnit(UnitPercent);
		usage->setMax(100);
		auto total = new AggregateSensor(this, QStringLiteral("totalVram"), QStringLiteral("All GPUs Total Memory"));
		total->setShortName(QStringLiteral("Total"));
		total->setMatchSensors(others, QStringLiteral("totalVram"));
		total->setUnit(UnitByte);
		auto used = new AggregateSensor(this, QStringLiteral("usedVram"), QStringLiteral("All GPUs Used Memory"));
		used->setShortName(QStringLiteral("Used"));
		used->setMatchSensors(others, QStringLiteral("usedVram"));
		used->setUnit(UnitByte);
		used->setMax(total);
	}
};

class Quest1GpuPlugin : public SensorPlugin
{
	Q_OBJECT
public:
	Quest1GpuPlugin(QObject *parent, const QVariantList &args)
		: SensorPlugin(parent, args)
	{
		auto container = new SensorContainer(QStringLiteral("gpu"), QStringLiteral("GPU"), this);
		if (!QFile::exists(QStringLiteral("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage")))
			return;
		m_gpu = new Gpu(container);
		new AllGpus(container);
	}

	QString providerName() const override
	{
		return QStringLiteral("gpu");
	}

	void update() override
	{
		if (m_gpu)
			m_gpu->update();
	}

private:
	Gpu *m_gpu = nullptr;
};

K_PLUGIN_CLASS_WITH_JSON(Quest1GpuPlugin, "metadata.json")

#include "quest1_gpu.moc"
