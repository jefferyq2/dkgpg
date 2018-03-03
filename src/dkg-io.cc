/*******************************************************************************
   This file is part of Distributed Privacy Guard (DKGPG).

 Copyright (C) 2018  Heiko Stamer <HeikoStamer@gmx.net>

   DKGPG is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   DKGPG is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with DKGPG; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*******************************************************************************/

// include headers
#ifdef HAVE_CONFIG_H
	#include "dkgpg_config.h"
#endif
#include "dkg-io.hh"

bool get_passphrase
	(const std::string &prompt, std::string &passphrase)
{
	struct termios old_term, new_term;
	
	// disable echo on stdin
	if (tcgetattr(fileno(stdin), &old_term) < 0)
	{
		perror("get_passphrase (tcgetattr)");
		return false;
	}
	new_term = old_term;
	new_term.c_lflag &= ~(ECHO | ISIG);
	new_term.c_lflag |= ECHONL;
	if (tcsetattr(fileno(stdin), TCSANOW, &new_term) < 0)
	{
		perror("get_passphrase (tcsetattr)");
		return false;
	}
	// read the passphrase
	std::cout << prompt.c_str() << ": ";
	std::getline(std::cin, passphrase);
	std::cin.clear();
	// enable echo on stdin
	if (tcsetattr(fileno(stdin), TCSANOW, &old_term) < 0)
	{
		perror("get_passphrase (tcsetattr)");
		return false;
	}
	return true;
}

bool read_key_file
	(const std::string &filename, std::string &result)
{
	// read the public/private key from file
	std::string line;
	std::stringstream key;
	std::ifstream ifs(filename.c_str(), std::ifstream::in);
	if (!ifs.is_open())
	{
		std::cerr << "ERROR: cannot open public/private key file" << std::endl;
		return false;
	}
	while (std::getline(ifs, line))
		key << line << std::endl;
	if (!ifs.eof())
	{
		ifs.close();
		std::cerr << "ERROR: reading public/private key file until EOF failed" << std::endl;
		return false;
	}
	ifs.close();
	result = key.str();
	return true;
}

bool read_binary_key_file
	(const std::string &filename, const tmcg_openpgp_armor_t type, std::string &result)
{
	// read the public/private key from file and convert to ASCII armor
	tmcg_openpgp_octets_t input;
	std::ifstream ifs(filename.c_str(), std::ifstream::in);
	if (!ifs.is_open())
	{
		std::cerr << "ERROR: cannot open public/private key file" << std::endl;
		return false;
	}
	char c;
	while (ifs.get(c))
		input.push_back(c);
	if (!ifs.eof())
	{
		ifs.close();
		std::cerr << "ERROR: reading public/private key file until EOF failed" << std::endl;
		return false;
	}
	ifs.close();
	CallasDonnerhackeFinneyShawThayerRFC4880::ArmorEncode(type, input, result);
	return true;
}

bool read_message
	(const std::string &filename, std::string &result)
{
	// read the (encrypted) message from file
	std::string line;
	std::stringstream msg;
	std::ifstream ifs(filename.c_str(), std::ifstream::in);
	if (!ifs.is_open())
	{
		std::cerr << "ERROR: cannot open input file" << std::endl;
		return false;
	}
	while (std::getline(ifs, line))
		msg << line << std::endl;
	if (!ifs.eof())
	{
		ifs.close();
		std::cerr << "ERROR: reading from input file until EOF failed" << std::endl;
		return false;
	}
	ifs.close();
	result = msg.str();
	return true;
}

bool read_binary_message
	(const std::string &filename, std::string &result)
{
	// read the (encrypted) message from file and convert to ASCII armor
	tmcg_openpgp_octets_t input;
	std::ifstream ifs(filename.c_str(), std::ifstream::in);
	if (!ifs.is_open())
	{
		std::cerr << "ERROR: cannot open input file" << std::endl;
		return false;
	}
	char c;
	while (ifs.get(c))
		input.push_back(c);
	if (!ifs.eof())
	{
		ifs.close();
		std::cerr << "ERROR: reading from input file until EOF failed" << std::endl;
		return false;
	}
	ifs.close();
	CallasDonnerhackeFinneyShawThayerRFC4880::ArmorEncode(TMCG_OPENPGP_ARMOR_MESSAGE, input, result);
	return true;
}

bool write_message
	(const std::string &filename, const tmcg_openpgp_octets_t &msg)
{
	// write out the (decrypted) message to file
	std::ofstream ofs(filename.c_str(), std::ofstream::out);
	if (!ofs.good())
	{
		std::cerr << "ERROR: cannot open output file" << std::endl;
		return false;
	}
	for (size_t i = 0; i < msg.size(); i++)
	{
		ofs << msg[i];
		if (!ofs.good())
		{
			ofs.close();
			std::cerr << "ERROR: writing to output file failed" << std::endl;
			return false;
		}
	}
	ofs.close();
	return true;
}

bool write_message
	(const std::string &filename, const std::string &msg)
{
	// write out the (decrypted) message to file
	std::ofstream ofs(filename.c_str(), std::ofstream::out);
	if (!ofs.good())
	{
		std::cerr << "ERROR: cannot open output file" << std::endl;
		return false;
	}
	for (size_t i = 0; i < msg.length(); i++)
	{
		ofs << msg[i];
		if (!ofs.good())
		{
			ofs.close();
			std::cerr << "ERROR: writing to output file failed" << std::endl;
			return false;
		}
	}
	ofs.close();
	return true;
}

bool lock_memory
	()
{
	if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
	{
		perror("lock_memory (mlockall)");
		return false;
	}
	return true;
}

bool unlock_memory
	()
{
	if (munlockall() < 0)
	{
		perror("unlock_memory (munlockall)");
		return false;
	}
	return true;
}
