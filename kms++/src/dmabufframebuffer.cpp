
#include <cstring>
#include <cerrno>

#include <stdexcept>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/dma-buf.h>

#include <kms++/kms++.h>

using namespace std;

namespace kms
{

DmabufFramebuffer::DmabufFramebuffer(Card& card, uint32_t width, uint32_t height, PixelFormat format,
				     vector<int> fds, vector<uint32_t> pitches, vector<uint32_t> offsets)
	: Framebuffer(card, width, height)
{
	int r;

	m_format = format;

	const PixelFormatInfo& format_info = get_pixel_format_info(format);

	m_num_planes = format_info.num_planes;

	for (int i = 0; i < format_info.num_planes; ++i) {
		FramebufferPlane& plane = m_planes[i];

		plane.prime_fd = fds[i];

		r = drmPrimeFDToHandle(card.fd(), fds[i], &plane.handle);
		if (r)
			throw invalid_argument(string("drmPrimeFDToHandle: ") + strerror(errno));

		plane.stride = pitches[i];
		plane.offset = offsets[i];
		plane.size = plane.stride * height;
		plane.map = 0;
	}

	uint32_t id;
	uint32_t bo_handles[4] = { m_planes[0].handle, m_planes[1].handle, m_planes[2].handle, m_planes[3].handle };
	r = drmModeAddFB2(card.fd(), width, height, (uint32_t)format,
			  bo_handles, pitches.data(), offsets.data(), &id, 0);
	if (r)
		throw invalid_argument(string("drmModeAddFB2 failed: ") + strerror(errno));

	set_id(id);
}

DmabufFramebuffer::~DmabufFramebuffer()
{
	drmModeRmFB(card().fd(), id());
}

uint8_t* DmabufFramebuffer::map(unsigned plane)
{
	FramebufferPlane& p = m_planes[plane];

	if (p.map)
		return p.map;

	p.map = (uint8_t *)mmap(0, p.size, PROT_READ | PROT_WRITE, MAP_SHARED,
				p.prime_fd, 0);
	if (p.map == MAP_FAILED)
		throw invalid_argument(string("mmap failed: ") + strerror(errno));

	return p.map;
}

int DmabufFramebuffer::prime_fd(unsigned plane)
{
	FramebufferPlane& p = m_planes[plane];

	return p.prime_fd;
}

void DmabufFramebuffer::begin_cpu_access(CpuAccess access)
{
	if (m_sync_flags != 0)
		throw runtime_error("begin_cpu sync already started");

	switch (access) {
	case CpuAccess::Read:
		m_sync_flags = DMA_BUF_SYNC_READ;
		break;
	case CpuAccess::Write:
		m_sync_flags = DMA_BUF_SYNC_WRITE;
		break;
	case CpuAccess::ReadWrite:
		m_sync_flags = DMA_BUF_SYNC_RW;
		break;
	}

	dma_buf_sync dbs {
		.flags = DMA_BUF_SYNC_START | m_sync_flags
	};

	for (uint32_t p = 0; p < m_num_planes; ++p) {
		int r = ioctl(prime_fd(p), DMA_BUF_IOCTL_SYNC, &dbs);
		if (r)
			throw runtime_error("DMA_BUF_IOCTL_SYNC failed");
	}
}

void DmabufFramebuffer::end_cpu_access()
{
	if (m_sync_flags == 0)
		throw runtime_error("begin_cpu sync not started");

	dma_buf_sync dbs {
		.flags = DMA_BUF_SYNC_END | m_sync_flags
	};

	for (uint32_t p = 0; p < m_num_planes; ++p) {
		int r = ioctl(prime_fd(p), DMA_BUF_IOCTL_SYNC, &dbs);
		if (r)
			throw runtime_error("DMA_BUF_IOCTL_SYNC failed");
	}

	m_sync_flags = 0;
}

}
