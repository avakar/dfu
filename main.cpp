#include <libyb/async/sync_runner.hpp>
#include <libyb/usb/usb_context.hpp>
#include <libyb/async/channel.hpp>
#include <libyb/async/timer.hpp>
#include <iostream>
#include <fstream>

#include "crc.h"

void print_help(char const * argv0)
{
	std::cout << "usage: " << argv0 << " [--vidpid <vidpid>] [--sn <sn>] { --list | <infile> }" << std::endl;
}

std::vector<yb::usb_interface>::const_iterator find_dfu_intf(yb::usb_config_descriptor const & desc)
{
	return std::find_if(desc.interfaces.begin(), desc.interfaces.end(), [](yb::usb_interface const & intf) -> bool {
		if (intf.altsettings.size() != 1)
			return false;

		yb::usb_interface_descriptor const & intf_desc = intf.altsettings[0];
		return intf_desc.bInterfaceClass == 0xfe || intf_desc.bInterfaceSubClass == 0x01
			|| (intf_desc.bInterfaceSubClass == 0x01 && intf_desc.bInterfaceSubClass == 0x02);
	});
}

struct dfu_suffix
{
	uint32_t dwCRC;
	uint8_t bLength;
	uint8_t ucDfuSignature[3];
	uint16_t bcdDFU;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
};

template <typename T>
T load_le(uint8_t const * buf, size_t size = sizeof(T))
{
	T res = 0;
	buf += size;
	for (size_t i = 0; i < size; ++i)
		res = (res << 8) | (*--buf);
	return res;
}

static dfu_suffix parse_dfu_suffix(uint8_t const * buf)
{
	dfu_suffix suffix;
	suffix.dwCRC = load_le<uint32_t>(buf + 12);
	suffix.bLength = buf[11];
	suffix.ucDfuSignature[0] = buf[10];
	suffix.ucDfuSignature[1] = buf[9];
	suffix.ucDfuSignature[2] = buf[8];
	suffix.bcdDevice = load_le<uint16_t>(buf);
	suffix.idProduct = load_le<uint16_t>(buf + 2);
	suffix.idVendor = load_le<uint16_t>(buf + 4);
	suffix.bcdDFU = load_le<uint16_t>(buf + 6);
	return suffix;
}

int main(int argc, char * argv[])
{
	uint32_t vidpid_filter = 0;
	std::string sn_filter;
	std::string infile;
	uint16_t detach_timeout = 5000;

	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--vidpid") == 0)
		{
			vidpid_filter = strtol(argv[++i], 0, 16);
		}
		else if (strcmp(argv[i], "--sn") == 0)
		{
			sn_filter = argv[++i];
		}
		else if (strcmp(argv[i], "--help") == 0)
		{
			print_help(argv[0]);
			return 0;
		}
		else if (strcmp(argv[i], "--detach-timeout") == 0)
		{
			detach_timeout = (uint16_t)strtol(argv[++i], 0, 0);
		}
		else if (argv[i][0] == '-')
		{
			std::cerr << "error: unknown option: " << argv[i] << std::endl;
			return 2;
		}
		else
		{
			infile = argv[i];
		}
	}

	yb::sync_runner runner;
#define await runner%

	yb::usb_context usb_ctx(runner);

	std::vector<yb::usb_device> devices = usb_ctx.list_devices();

	auto filter = [&](yb::usb_device const & device) -> bool {
		if (vidpid_filter != 0 && vidpid_filter != device.vidpid())
			return true;

		if (!sn_filter.empty() && sn_filter != device.serial_number())
			return true;

		yb::usb_config_descriptor const & desc = device.get_config_descriptor();
		if (desc.interfaces.empty())
			return true;

		return find_dfu_intf(desc) == desc.interfaces.end();
	};

	devices.erase(
		std::remove_if(devices.begin(), devices.end(), filter),
		devices.end());

	for (auto && dev: devices)
	{
		std::cout << dev.product() << ", " << std::hex << dev.vidpid() << ", " << dev.serial_number() << std::endl;
	}

	if (!infile.empty())
	{
		std::ifstream fin(infile, std::ios::binary);
		if (!fin.is_open())
		{
			std::cerr << "error: cannot open the input file: " << infile << std::endl;
			return 2;
		}

		uint8_t suffix_buf[16];
		if (!fin.seekg(-16, std::ios::end)
			|| !fin.read((char *)suffix_buf, sizeof suffix_buf))
		{
			std::cerr << "error: failed to read the input file: " << infile << std::endl;
			return 2;
		}

		dfu_suffix suf = parse_dfu_suffix(suffix_buf);
		if (suf.ucDfuSignature[0] != 'D' || suf.ucDfuSignature[1] != 'F' || suf.ucDfuSignature[2] != 'U'
			|| suf.bcdDFU != 0x100 || suf.bLength != 16)
		{
			std::cerr << "error: the file does not contain a valid DFU suffix" << std::endl;
			return 3;
		}

		std::streamoff fw_size = (std::streamoff)fin.tellg() - suf.bLength;

		if (devices.empty())
		{
			std::cerr << "error: no DFU devices found" << std::endl;
			return 1;
		}

		if (devices.size() != 1)
		{
			std::cerr << "error: too many devices found, use --vidpid or --sn to filter them" << std::endl;
			return 1;
		}

		yb::usb_device dev = devices[0];
		yb::usb_device_descriptor const & dev_desc = dev.descriptor();

		if ((suf.idProduct != 0xFFFF && suf.idProduct != dev_desc.idProduct)
			|| (suf.idVendor != 0xFFFF && suf.idVendor != dev_desc.idVendor)
			|| (suf.bcdDevice != 0xFFFF && suf.bcdDevice != dev_desc.bcdDevice))
		{
			std::cerr << "error: the firmware is not meant for this device" << std::endl;
			return 4;
		}

		fin.seekg(0);

		uint32_t fin_crc = 0;
		for (size_t offs = 0; offs < fw_size; )
		{
			uint8_t buf[64 * 1024];
			size_t chunk = (std::min)(size_t(fw_size - offs), sizeof buf);
			if (!fin.read((char *)buf, chunk))
			{
				std::cerr << "error: failed to read from the input file" << std::endl;
				return 5;
			}

			fin_crc = crc(buf, buf + chunk, fin_crc);
			offs += chunk;
		}

		fin_crc = crc(suffix_buf, suffix_buf + 12, fin_crc);
		if (suf.dwCRC != fin_crc)
		{
			std::cerr << "error: CRC check on the inpuc file failed" << std::endl;
			return 6;
		}

		uint32_t vidpid = dev.vidpid();
		std::string sn = dev.serial_number();

		static yb::usb_control_code_t const cmd_dfu_detach = { 0x21, 0x00 };
		static yb::usb_control_code_t const cmd_dfu_dnload = { 0x21, 0x01 };
		static yb::usb_control_code_t const cmd_dfu_abort = { 0x21, 0x06 };
		static yb::usb_control_code_t const cmd_dfu_getstatus = { 0xa1, 0x03 };
		static yb::usb_control_code_t const cmd_dfu_clrstatus = { 0x21, 0x04 };

		{
			auto const & config_desc = dev.get_config_descriptor();
			auto const & intf = find_dfu_intf(config_desc)->altsettings[0];
			if (intf.bInterfaceProtocol == 0x01)
			{
				yb::channel<yb::usb_device> dev_chan = yb::channel<yb::usb_device>::create_finite<1>();

				{
					yb::usb_monitor mon = usb_ctx.monitor([vidpid, sn, dev_chan](yb::usb_plugin_event const & e) {
						if (e.action != yb::usb_plugin_event::a_add || e.dev.empty() || e.dev.vidpid() != vidpid || e.dev.serial_number() != sn)
							return;

						yb::usb_config_descriptor conf_desc = e.dev.get_config_descriptor();
						if (conf_desc.interfaces.size() != 1 || conf_desc.interfaces[0].altsettings.size() != 1)
							return;

						yb::usb_interface_descriptor const & intf_desc = conf_desc.interfaces[0].altsettings[0];
						if (intf_desc.bInterfaceClass == 0xfe && intf_desc.bInterfaceSubClass == 0x01 && intf_desc.bInterfaceProtocol == 0x02)
							dev_chan.send_sync(e.dev);
					});

					await dev.control_write(cmd_dfu_detach, detach_timeout, intf.bInterfaceNumber, 0, 0);
					await dev.reset_device();

					std::cout << "Waiting for the device to reatach in DFU mode...\n";
					dev = await dev_chan.receive();
				}
			}
		}

		// `dev` is now the device in DFU mode
		auto const & config_desc = dev.get_config_descriptor();
		auto const & intf = config_desc.interfaces[0].altsettings[0];

		std::vector<uint8_t> const * fndesc = 0;
		for (auto && extra : intf.extra_descriptors)
		{
			if (extra.size() == 9 && extra[1] == 0x21)
			{
				fndesc = &extra;
				break;
			}
		}

		if (!fndesc)
		{
			std::cerr << "error: the device doesn't have a valid DFU descriptor\n";
			return 4;
		}

		uint8_t bmAttributes = (*fndesc)[2];
		static uint8_t const bitCanDnload = (1 << 0);
		static uint8_t const bitCanUpload = (1 << 1);
		static uint8_t const bitManifestationTolerant = (1 << 2);
		static uint8_t const bitWillDetach = (1 << 3);

		uint16_t wDetachTimeout = load_le<uint16_t>(fndesc->data() + 3);
		uint16_t wTransferSize = load_le<uint16_t>(fndesc->data() + 5);
		uint16_t bcdDFUVersion = load_le<uint16_t>(fndesc->data() + 7);

		enum dfu_state_t
		{
			app_idle,
			app_detach,
			dfu_idle,
			dfu_dnload_sync,
			dfu_dnbusy,
			dfu_dnload_idle,
			dfu_manifest_sync,
			dfu_manifest,
			dfu_manifest_wait_reset,
			dfu_upload_idle,
			dfu_error,
		};

		for (;;)
		{
			uint8_t status[6];
			size_t read_count = await dev.control_read(cmd_dfu_getstatus, 0, intf.bInterfaceNumber, status, sizeof status);
			if (read_count != sizeof status)
			{
				std::cerr << "error: DFU_GETSTATUS returned wrong length" << std::endl;
				return 7;
			}

			if (status[4] > dfu_error)
			{
				std::cerr << "error: DFU_GETSTATUS returned invalid data" << std::endl;
				return 8;
			}

			if (status[4] == dfu_error)
			{
				await dev.control_write(cmd_dfu_clrstatus, 0, intf.bInterfaceNumber, 0, 0);
				continue;
			}

			if (status[4] == dfu_idle)
				break;

			await dev.control_write(cmd_dfu_abort, 0, intf.bInterfaceNumber, 0, 0);
		}

		fin.seekg(0);
		std::vector<uint8_t> buf(wTransferSize);

		uint16_t block_num = 0;
		std::streamoff total_size = fw_size;
		std::streamoff transferred_size = 0;
		while (fw_size)
		{
			std::cout << "\r" << std::dec << transferred_size << "/" << total_size << std::flush;

			uint16_t chunk = fw_size > wTransferSize? wTransferSize: (uint16_t)fw_size;
			fin.read((char *)buf.data(), chunk);
			await dev.control_write(cmd_dfu_dnload, block_num++, intf.bInterfaceNumber, buf.data(), chunk);
			fw_size -= chunk;

			transferred_size += chunk;

			for (;;)
			{
				uint8_t status[6];
				size_t read_count = await dev.control_read(cmd_dfu_getstatus, 0, intf.bInterfaceNumber, status, sizeof status);
				if (read_count != sizeof status)
				{
					std::cerr << "error: DFU_GETSTATUS returned wrong length" << std::endl;
					return 7;
				}

				if (status[4] == dfu_error)
				{
					std::cerr << "error: an error has occurred" << std::endl;
					return 9;
				}

				if (status[4] != dfu_dnbusy && status[4] != dfu_dnload_idle)
				{
					std::cerr << "error: DFU_GETSTATUS returned invalid data" << std::endl;
					return 8;
				}

				if (status[4] == dfu_dnload_idle)
					break;

				
				uint32_t poll_timeout = load_le<uint32_t>(status + 1, 3);
				if (poll_timeout)
					await yb::wait_ms(poll_timeout);
			}
		}

		std::cout << "\r" << std::dec << transferred_size << "/" << total_size << "\n";

		await dev.control_write(cmd_dfu_dnload, block_num++, intf.bInterfaceNumber, 0, 0);

		for (;;)
		{
			uint8_t status[6];
			size_t read_count = await dev.control_read(cmd_dfu_getstatus, 0, intf.bInterfaceNumber, status, sizeof status);
			if (read_count != sizeof status)
			{
				std::cerr << "error: DFU_GETSTATUS returned wrong length" << std::endl;
				return 7;
			}

			if (status[4] == dfu_error)
			{
				std::cerr << "error: an error has occurred" << std::endl;
				return 9;
			}

			if (status[4] != dfu_idle && status[4] != dfu_manifest)
			{
				std::cerr << "error: DFU_GETSTATUS returned invalid data" << std::endl;
				return 8;
			}

			if (status[4] == dfu_idle)
			{
				await dev.reset_device();
				break;
			}

			uint32_t poll_timeout = load_le<uint32_t>(status + 1, 3);
			await yb::wait_ms(poll_timeout);

			if ((bmAttributes & bitManifestationTolerant) == 0)
			{
				await dev.reset_device();
				break;
			}
		}
	}
}
