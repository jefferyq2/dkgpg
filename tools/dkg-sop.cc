/*******************************************************************************
   This file is part of Distributed Privacy Guard (DKGPG).

 Copyright (C) 2019, 2020  Heiko Stamer <HeikoStamer@gmx.net>

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

// [DKG20] https://datatracker.ietf.org/doc/draft-dkg-openpgp-stateless-cli/

// include headers
#ifdef HAVE_CONFIG_H
	#include "dkgpg_config.h"
#endif

#include <string>
#include <vector>
#include <ctime>
#include <cstdio>

#include <zlib.h>
#ifdef LIBBZ
#include <bzlib.h>
#endif

#include <libTMCG.hh>

#include "dkg-io.hh"
#include "dkg-common.hh"

int opt_verbose = 0;
bool opt_rfc4880bis = true;
bool opt_armor = true;
bool opt_as_binary = true;
bool opt_as_text = false;
bool opt_as_mime = false;
time_t opt_not_before = 0;
time_t opt_not_after = 0;
std::vector<std::string> opt_with_password;
std::vector<std::string> opt_sign_with;
std::vector<std::string> opt_with_session_key;
std::string opt_verify_out;
std::vector<std::string> opt_verify_with;

int encrypt_ret = -1;

bool generate
	(const std::vector<std::string> &args,
	 const tmcg_openpgp_secure_string_t &passphrase) 
{
	time_t keytime = time(NULL); // current time
	time_t keyexptime = 0; // no expiration
	std::stringstream crss;
	mpz_t cache[TMCG_MAX_SSRANDOMM_CACHE], cache_mod;
	size_t cache_avail = 0;
	// check magic bytes of CRS (common reference string)
	if (TMCG_ParseHelper::cm(crs, "fips-crs", '|'))
	{
		if (opt_verbose)
		{
			std::cerr << "INFO: verifying domain parameters (according" <<
				" to FIPS 186-4 section A.1.1.2)" << std::endl;
		}
	}
	else
	{
		std::cerr << "ERROR: wrong type of CRS detected" << std::endl;
		return false;
	}
	// extract p, q, g from CRS
	mpz_t fips_p, fips_q, fips_g;
	mpz_init(fips_p), mpz_init(fips_q), mpz_init(fips_g);
	if (!pqg_extract(crs, true, opt_verbose, fips_p, fips_q, fips_g, crss))
	{
		mpz_clear(fips_p), mpz_clear(fips_q), mpz_clear(fips_g);
		return false;
	}
	// initialize cache
	if (opt_verbose)
	{
		std::cerr << "INFO: We need a lot of entropy for key generation." <<
			std::endl;
		std::cerr << "Please use other programs, move the mouse, and" <<
			" type on your keyboard: " << std::endl;
	}
	tmcg_mpz_ssrandomm_cache_init(cache, cache_mod, cache_avail, 2, fips_q);
	if (opt_verbose)
		std::cerr << "Thank you!" << std::endl;
	mpz_clear(fips_p), mpz_clear(fips_q), mpz_clear(fips_g);
	// create a VTMF instance from CRS
	BarnettSmartVTMF_dlog *vtmf = new BarnettSmartVTMF_dlog(crss,
		TMCG_DDH_SIZE, TMCG_DLSE_SIZE, false); // without VTMF-verifiable g
	// check the VTMF instance
	if (!vtmf->CheckGroup())
	{
		std::cerr << "ERROR: group G from CRS is bad" << std::endl;
		delete vtmf;
		return false;
	}
	// select hash algorithm for OpenPGP based on |q| (size in bit)
	tmcg_openpgp_hashalgo_t hashalgo = TMCG_OPENPGP_HASHALGO_UNKNOWN;
	if (mpz_sizeinbase(vtmf->q, 2L) == 256)
		hashalgo = TMCG_OPENPGP_HASHALGO_SHA256; // SHA256 (alg 8)
	else if (mpz_sizeinbase(vtmf->q, 2L) == 384)
		hashalgo = TMCG_OPENPGP_HASHALGO_SHA384; // SHA384 (alg 9)
	else if (mpz_sizeinbase(vtmf->q, 2L) == 512)
		hashalgo = TMCG_OPENPGP_HASHALGO_SHA512; // SHA512 (alg 10)
	else
	{
		std::cerr << "ERROR: selecting hash algorithm failed for |q| = " <<
			mpz_sizeinbase(vtmf->q, 2L) << std::endl;
		delete vtmf;
		return false;
	}
	// generate a non-shared DSA primary key
	mpz_t dsa_y, dsa_x;
	mpz_init(dsa_y), mpz_init(dsa_x);
	tmcg_mpz_ssrandomm_cache(cache, cache_mod, cache_avail, dsa_x, vtmf->q);
	tmcg_mpz_spowm(dsa_y, vtmf->g, dsa_x, vtmf->p);
	// extract parameters for OpenPGP key structures
	tmcg_openpgp_octets_t pub, sec, dirsig;
	tmcg_openpgp_octets_t sub, ssb, subsig, dsaflags, elgflags, issuer;
	tmcg_openpgp_octets_t pub_hashing, sub_hashing;
	tmcg_openpgp_octets_t dirsig_hashing, dirsig_left;
	tmcg_openpgp_octets_t subsig_hashing, subsig_left;
	tmcg_openpgp_octets_t hash, empty;
	time_t sigtime;
	gcry_sexp_t key;
	gcry_mpi_t p, q, g, y, x, r, s;
	gcry_error_t ret;
	p = gcry_mpi_new(2048);
	if (!tmcg_mpz_get_gcry_mpi(p, vtmf->p))
	{
		std::cerr << "ERROR: tmcg_mpz_get_gcry_mpi() failed" << std::endl;
		mpz_clear(dsa_y), mpz_clear(dsa_x);
		gcry_mpi_release(p);
		delete vtmf;
		return false;
	}
	q = gcry_mpi_new(2048);
	if (!tmcg_mpz_get_gcry_mpi(q, vtmf->q))
	{
		std::cerr << "ERROR: tmcg_mpz_get_gcry_mpi() failed" << std::endl;
		mpz_clear(dsa_y), mpz_clear(dsa_x);
		gcry_mpi_release(p);
		gcry_mpi_release(q);
		delete vtmf;
		return false;
	}
	g = gcry_mpi_new(2048);
	if (!tmcg_mpz_get_gcry_mpi(g, vtmf->g))
	{
		std::cerr << "ERROR: tmcg_mpz_get_gcry_mpi() failed" << std::endl;
		mpz_clear(dsa_y), mpz_clear(dsa_x);
		gcry_mpi_release(p);
		gcry_mpi_release(q);
		gcry_mpi_release(g);
		delete vtmf;
		return false;
	}
	y = gcry_mpi_new(2048);
	if (!tmcg_mpz_get_gcry_mpi(y, dsa_y))
	{
		std::cerr << "ERROR: tmcg_mpz_get_gcry_mpi() failed" << std::endl;
		mpz_clear(dsa_y), mpz_clear(dsa_x);
		gcry_mpi_release(p);
		gcry_mpi_release(q);
		gcry_mpi_release(g);
		gcry_mpi_release(y);
		delete vtmf;
		return false;
	}
	x = gcry_mpi_snew(2048);
	if (!tmcg_mpz_get_gcry_mpi(x, dsa_x))
	{
		std::cerr << "ERROR: tmcg_mpz_get_gcry_mpi() failed" << std::endl;
		mpz_clear(dsa_y), mpz_clear(dsa_x);
		gcry_mpi_release(p);
		gcry_mpi_release(q);
		gcry_mpi_release(g);
		gcry_mpi_release(y);
		gcry_mpi_release(x);
		delete vtmf;
		return false;
	}
	mpz_clear(dsa_y), mpz_clear(dsa_x);
	size_t erroff;
	ret = gcry_sexp_build(&key, &erroff,
		"(key-data (public-key (dsa (p %M) (q %M) (g %M) (y %M)))"
		" (private-key (dsa (p %M) (q %M) (g %M) (y %M) (x %M))))",
		p, q, g, y, p, q, g, y, x);
	if (ret)
	{
		std::cerr << "ERROR: gcry_sexp_build() failed" << std::endl;
		gcry_mpi_release(p);
		gcry_mpi_release(q);
		gcry_mpi_release(g);
		gcry_mpi_release(y);
		gcry_mpi_release(x);
		delete vtmf;
		return false;
	}
	CallasDonnerhackeFinneyShawThayerRFC4880::
		PacketPubEncode(keytime, TMCG_OPENPGP_PKALGO_DSA, p, q, g, y, pub);
	CallasDonnerhackeFinneyShawThayerRFC4880::
		PacketSecEncode(keytime, TMCG_OPENPGP_PKALGO_DSA, p, q, g, y, x,
			passphrase, sec);
	CallasDonnerhackeFinneyShawThayerRFC4880::
		PacketBodyExtract(pub, 0, pub_hashing);
	CallasDonnerhackeFinneyShawThayerRFC4880::
		FingerprintCompute(pub_hashing, issuer);
	std::vector<tmcg_openpgp_octets_t> uid, uidsig;
	uid.resize(args.size());
	uidsig.resize(args.size());
	for (size_t i = 0; i < args.size(); i++)
	{
		CallasDonnerhackeFinneyShawThayerRFC4880::
			PacketUidEncode(args[i], uid[i]);
	}
	dsaflags.push_back(0x01 | 0x02);
	sigtime = time(NULL); // current time
	CallasDonnerhackeFinneyShawThayerRFC4880::
		PacketSigPrepareDesignatedRevoker(TMCG_OPENPGP_PKALGO_DSA, hashalgo,
			sigtime, dsaflags, issuer, (tmcg_openpgp_pkalgo_t)0, empty,
			opt_rfc4880bis, dirsig_hashing);
	hash.clear();
	CallasDonnerhackeFinneyShawThayerRFC4880::
		KeyHash(pub_hashing, dirsig_hashing, hashalgo, hash, dirsig_left);
	r = gcry_mpi_new(2048);
	s = gcry_mpi_new(2048);
	ret = CallasDonnerhackeFinneyShawThayerRFC4880::
		AsymmetricSignDSA(hash, key, r, s);
	if (ret)
	{
		std::cerr << "ERROR: AsymmetricSignDSA() failed" << std::endl;
		gcry_mpi_release(p);
		gcry_mpi_release(q);
		gcry_mpi_release(g);
		gcry_mpi_release(y);
		gcry_mpi_release(x);
		gcry_mpi_release(r);
		gcry_mpi_release(s);
		gcry_sexp_release(key);
		delete vtmf;
		return false;
	}
	CallasDonnerhackeFinneyShawThayerRFC4880::
		PacketSigEncode(dirsig_hashing, dirsig_left, r, s, dirsig);
	gcry_mpi_release(r);
	gcry_mpi_release(s);
	for (size_t i = 0; i < uid.size(); i++)
	{
		tmcg_openpgp_octets_t uidsig_hashing, uidsig_left;
		CallasDonnerhackeFinneyShawThayerRFC4880::
			PacketSigPrepareSelfSignature(
				TMCG_OPENPGP_SIGNATURE_POSITIVE_CERTIFICATION, 
				TMCG_OPENPGP_PKALGO_DSA, hashalgo, sigtime, keyexptime,
				dsaflags, issuer, opt_rfc4880bis, uidsig_hashing); 
		hash.clear();
		CallasDonnerhackeFinneyShawThayerRFC4880::
			CertificationHash(pub_hashing, args[i], empty, uidsig_hashing,
			hashalgo, hash, uidsig_left);
		r = gcry_mpi_new(2048);
		s = gcry_mpi_new(2048);
		ret = CallasDonnerhackeFinneyShawThayerRFC4880::
			AsymmetricSignDSA(hash, key, r, s);
		if (ret)
		{
			std::cerr << "ERROR: AsymmetricSignDSA() failed" << std::endl;
			gcry_mpi_release(p);
			gcry_mpi_release(q);
			gcry_mpi_release(g);
			gcry_mpi_release(y);
			gcry_mpi_release(x);
			gcry_mpi_release(r);
			gcry_mpi_release(s);
			gcry_sexp_release(key);
			delete vtmf;
			return false;
		}
		CallasDonnerhackeFinneyShawThayerRFC4880::
			PacketSigEncode(uidsig_hashing, uidsig_left, r, s, uidsig[i]);
		gcry_mpi_release(r);
		gcry_mpi_release(s);
	}
	gcry_mpi_release(x);
	gcry_mpi_release(y);
	// generate a non-shared ElGamal subkey with same domain parameter set
	mpz_t elg_y, elg_x;
	mpz_init(elg_y), mpz_init(elg_x);
	tmcg_mpz_ssrandomm_cache(cache, cache_mod, cache_avail, elg_x, vtmf->q);
	tmcg_mpz_spowm(elg_y, vtmf->g, elg_x, vtmf->p);
	// extract further parameters for OpenPGP key structures
	y = gcry_mpi_new(2048);
	if (!tmcg_mpz_get_gcry_mpi(y, elg_y))
	{
		std::cerr << "ERROR: tmcg_mpz_get_gcry_mpi() failed" << std::endl;
		mpz_clear(elg_y), mpz_clear(elg_x);
		gcry_mpi_release(p);
		gcry_mpi_release(q);
		gcry_mpi_release(g);
		gcry_mpi_release(y);
		gcry_sexp_release(key);
		delete vtmf;
		return false;
	}
	x = gcry_mpi_snew(2048);
	if (!tmcg_mpz_get_gcry_mpi(x, elg_x))
	{
		std::cerr << "ERROR: tmcg_mpz_get_gcry_mpi() failed" << std::endl;
		mpz_clear(elg_y), mpz_clear(elg_x);
		gcry_mpi_release(p);
		gcry_mpi_release(q);
		gcry_mpi_release(g);
		gcry_mpi_release(y);
		gcry_mpi_release(x);
		delete vtmf;
		return false;
	}
	mpz_clear(elg_y), mpz_clear(elg_x);
	CallasDonnerhackeFinneyShawThayerRFC4880::
		PacketSubEncode(keytime, TMCG_OPENPGP_PKALGO_ELGAMAL, p, q, g, y, sub);
	CallasDonnerhackeFinneyShawThayerRFC4880::
		PacketSsbEncode(keytime, TMCG_OPENPGP_PKALGO_ELGAMAL, p, q, g, y,
			x, passphrase, ssb);
	gcry_mpi_release(x);
	gcry_mpi_release(y);
	elgflags.push_back(0x04 | 0x08);
	sigtime = time(NULL); // current time
	// Subkey Binding Signature (0x18) of sub
	CallasDonnerhackeFinneyShawThayerRFC4880::
		PacketSigPrepareSelfSignature(TMCG_OPENPGP_SIGNATURE_SUBKEY_BINDING,
			TMCG_OPENPGP_PKALGO_DSA, hashalgo, sigtime, keyexptime, elgflags,
			issuer, opt_rfc4880bis, subsig_hashing);
	CallasDonnerhackeFinneyShawThayerRFC4880::
		PacketBodyExtract(sub, 0, sub_hashing);
	hash.clear();
	CallasDonnerhackeFinneyShawThayerRFC4880::
		KeyHash(pub_hashing, sub_hashing, subsig_hashing, hashalgo, hash,
			subsig_left);
	r = gcry_mpi_new(2048);
	s = gcry_mpi_new(2048);
	ret = CallasDonnerhackeFinneyShawThayerRFC4880::
		AsymmetricSignDSA(hash, key, r, s);
	if (ret)
	{
		std::cerr << "ERROR: AsymmetricSignDSA() failed" << std::endl;
		gcry_mpi_release(p);
		gcry_mpi_release(q);
		gcry_mpi_release(g);
		gcry_mpi_release(r);
		gcry_mpi_release(s);
		gcry_sexp_release(key);
		delete vtmf;
		return false;
	}
	CallasDonnerhackeFinneyShawThayerRFC4880::
		PacketSigEncode(subsig_hashing, subsig_left, r, s, subsig);
	gcry_mpi_release(r);
	gcry_mpi_release(s);
	// release
	gcry_mpi_release(p);
	gcry_mpi_release(q);
	gcry_mpi_release(g);
	gcry_sexp_release(key);
	// release the VTMF instance
	delete vtmf;
	// release cache
	tmcg_mpz_ssrandomm_cache_done(cache, cache_mod, cache_avail);
	// produce the output
	tmcg_openpgp_octets_t all;
	all.insert(all.end(), sec.begin(), sec.end());
	all.insert(all.end(), dirsig.begin(), dirsig.end());
	for (size_t i = 0; i < uid.size(); i++)
	{
		all.insert(all.end(), uid[i].begin(), uid[i].end());
		all.insert(all.end(), uidsig[i].begin(), uidsig[i].end());
	}
	all.insert(all.end(), ssb.begin(), ssb.end());
	all.insert(all.end(), subsig.begin(), subsig.end());
	if (opt_armor)
	{
		std::string armor;
		CallasDonnerhackeFinneyShawThayerRFC4880::
			ArmorEncode(TMCG_OPENPGP_ARMOR_PRIVATE_KEY_BLOCK, all, armor);
		std::cout << armor << std::endl;
	}
	else
	{
		for (size_t i = 0; i < all.size(); i++)
			std::cout << all[i];
	}
	return true;
}

bool extract
	(const tmcg_openpgp_secure_string_t &passphrase)
{
	// read and parse the private key
	TMCG_OpenPGP_Prvkey *prv = NULL;
	if (opt_armor)
	{
		std::string armored_key;
		read_stdin("-----END PGP PRIVATE KEY BLOCK-----" , armored_key, false);
		if (!CallasDonnerhackeFinneyShawThayerRFC4880::
			PrivateKeyBlockParse(armored_key, opt_verbose, passphrase, prv))
		{
			return false;
		}
	}
	else
	{
		tmcg_openpgp_octets_t key;
		char c;
		while (std::cin.get(c))
			key.push_back(c);
		if (!CallasDonnerhackeFinneyShawThayerRFC4880::
			PrivateKeyBlockParse(key, opt_verbose, passphrase, prv))
		{
			return false;
		}
	}
	// export the public key
	tmcg_openpgp_octets_t all;
	prv->pub->Export(all);
	delete prv;
	// output the result
	if (opt_armor)
	{
		std::string armor;
		CallasDonnerhackeFinneyShawThayerRFC4880::
			ArmorEncode(TMCG_OPENPGP_ARMOR_PUBLIC_KEY_BLOCK, all, armor);
		std::cout << armor << std::endl;
	}
	else
	{
		for (size_t i = 0; i < all.size(); i++)
			std::cout << all[i];
	}
	return true;
}

bool sign
	(const std::vector<std::string> &args,
	 const tmcg_openpgp_secure_string_t &passphrase,
	 const tmcg_openpgp_octets_t &data)
{
	tmcg_openpgp_octets_t sigs;
	for (size_t i = 0; i < args.size(); i++)
	{
		std::string armored_key;
		if (!autodetect_file(args[i], TMCG_OPENPGP_ARMOR_PRIVATE_KEY_BLOCK,
			armored_key))
		{
			return false;
		}
		TMCG_OpenPGP_Prvkey *prv = NULL;
		if (!CallasDonnerhackeFinneyShawThayerRFC4880::
			PrivateKeyBlockParse(armored_key, opt_verbose, passphrase, prv))
		{
			return false;
		}
		time_t sigtime = time(NULL); // current time, fixed hash algo SHA2-512
		tmcg_openpgp_hashalgo_t hashalgo = TMCG_OPENPGP_HASHALGO_SHA512;
		tmcg_openpgp_octets_t trailer, hash, left;
		bool hret = false;
		if (opt_as_text)
		{
			CallasDonnerhackeFinneyShawThayerRFC4880::
				PacketSigPrepareDetachedSignature(
					TMCG_OPENPGP_SIGNATURE_CANONICAL_TEXT_DOCUMENT,
					prv->pub->pkalgo, hashalgo, sigtime, 0, "",
					prv->pub->fingerprint, trailer);
			hret = CallasDonnerhackeFinneyShawThayerRFC4880::
				TextDocumentHash(data, trailer, hashalgo, hash, left);
		}
		else
		{
			CallasDonnerhackeFinneyShawThayerRFC4880::
				PacketSigPrepareDetachedSignature(
					TMCG_OPENPGP_SIGNATURE_BINARY_DOCUMENT, prv->pub->pkalgo,
					hashalgo, sigtime, 0, "", prv->pub->fingerprint, trailer);
			hret = CallasDonnerhackeFinneyShawThayerRFC4880::
				BinaryDocumentHash(data, trailer, hashalgo, hash, left);
		}
		if (!hret)
		{
			std::cerr << "ERROR: [Text|Binary]DocumentHash() failed" <<
				std::endl;
			delete prv;
			return false;
		}
		gcry_error_t ret;
		gcry_mpi_t r, s;
		r = gcry_mpi_new(2048);
		s = gcry_mpi_new(2048);
		switch (prv->pkalgo)
		{
			case TMCG_OPENPGP_PKALGO_RSA:
			case TMCG_OPENPGP_PKALGO_RSA_SIGN_ONLY:
				ret = CallasDonnerhackeFinneyShawThayerRFC4880::
					AsymmetricSignRSA(hash, prv->private_key, hashalgo, s);
				break;
			case TMCG_OPENPGP_PKALGO_DSA:
				ret = CallasDonnerhackeFinneyShawThayerRFC4880::
					AsymmetricSignDSA(hash, prv->private_key, r, s);
				break;
			case TMCG_OPENPGP_PKALGO_ECDSA:
				ret = CallasDonnerhackeFinneyShawThayerRFC4880::
					AsymmetricSignECDSA(hash, prv->private_key, r, s);
				break;
			case TMCG_OPENPGP_PKALGO_EDDSA:
				ret = CallasDonnerhackeFinneyShawThayerRFC4880::
					AsymmetricSignEdDSA(hash, prv->private_key, r, s);
				break;
			default:
				std::cerr << "ERROR: public-key algorithm " <<
					(int)prv->pkalgo << " not supported" << std::endl;
				gcry_mpi_release(r), gcry_mpi_release(s);
				delete prv;
				return false;
		}
		if (ret)
		{
			std::cerr << "ERROR: signing of hash value failed " <<
				"(rc = " << gcry_err_code(ret) << ", str = " <<
				gcry_strerror(ret) << ")" << std::endl;
			gcry_mpi_release(r), gcry_mpi_release(s);
			delete prv;
			return false;
		}
		switch (prv->pkalgo)
		{
			case TMCG_OPENPGP_PKALGO_RSA:
			case TMCG_OPENPGP_PKALGO_RSA_SIGN_ONLY:
				CallasDonnerhackeFinneyShawThayerRFC4880::
					PacketSigEncode(trailer, left, s, sigs);
				break;
			case TMCG_OPENPGP_PKALGO_DSA:
			case TMCG_OPENPGP_PKALGO_ECDSA:
			case TMCG_OPENPGP_PKALGO_EDDSA:
				CallasDonnerhackeFinneyShawThayerRFC4880::
					PacketSigEncode(trailer, left, r, s, sigs);
				break;
			default:
				std::cerr << "ERROR: public-key algorithm " <<
					(int)prv->pkalgo << " not supported" << std::endl;
				gcry_mpi_release(r), gcry_mpi_release(s);
				delete prv;
				return false;
		}
		gcry_mpi_release(r), gcry_mpi_release(s);
		delete prv;
	}
	// output the result
	if (opt_armor)
	{
		std::string armor;
		CallasDonnerhackeFinneyShawThayerRFC4880::
			ArmorEncode(TMCG_OPENPGP_ARMOR_SIGNATURE, sigs, armor);
		std::cout << armor << std::endl;
	}
	else
	{
		for (size_t i = 0; i < sigs.size(); i++)
			std::cout << sigs[i];
	}
	return true;
}

bool verify
	(const std::vector<std::string> &args,
	 const tmcg_openpgp_octets_t &data,
	 std::vector<std::string> &verifications)
{
	// read the signature(s) 
	std::string armored_signatures;
	if (!autodetect_file(args[0], TMCG_OPENPGP_ARMOR_SIGNATURE,
		armored_signatures))
	{
		return false;
	}
	// parse the signature(s)
	TMCG_OpenPGP_Signatures sigs;
	bool parse_ok = CallasDonnerhackeFinneyShawThayerRFC4880::
		SignaturesParse(armored_signatures, opt_verbose, sigs);
	if (!parse_ok)
	{
		std::cerr << "ERROR: cannot parse resp. use the" <<
			" provided signature(s)" << std::endl;
		return false;
	}
	// verify the signature(s)
	for (size_t i = 0; i < sigs.size(); i++)
	{
		if (opt_verbose)
			sigs[i]->PrintInfo();
		// 5. signature time (signatures made before or after are not valid)
		if ((sigs[i]->creationtime < opt_not_before) ||
			((opt_not_after > 0) && (sigs[i]->creationtime > opt_not_after)))
		{
			std::cerr << "WARNING: creation time of signature" <<
				" #" << i << " is outside provided range;" <<
				" signature ignored" << std::endl;
			continue;
		}
		// 6. hash algorithm (reject broken hash algorithms)
		if ((sigs[i]->hashalgo == TMCG_OPENPGP_HASHALGO_MD5) ||
		    (sigs[i]->hashalgo == TMCG_OPENPGP_HASHALGO_SHA1) ||
		    (sigs[i]->hashalgo == TMCG_OPENPGP_HASHALGO_RMD160))
		{
			std::string hashname;
			CallasDonnerhackeFinneyShawThayerRFC4880::
				AlgorithmHashTextName(sigs[i]->hashalgo, hashname);
			std::cerr << "WARNING: broken hash algorithm " <<
				hashname << " used for signature #" << i <<
				"; signature ignored" << std::endl;
			continue;
		}
		// all provided 
		for (size_t j = 1; j < args.size(); j++)
		{
			std::string armored_pubkey;
			if (!autodetect_file(args[j], TMCG_OPENPGP_ARMOR_PUBLIC_KEY_BLOCK,
				armored_pubkey))
			{
				for (size_t k = 0; k < sigs.size(); k++)
					delete sigs[k];
				return false;
			}
			TMCG_OpenPGP_Keyring *ring = new TMCG_OpenPGP_Keyring();
			TMCG_OpenPGP_Pubkey *primary = NULL;
			parse_ok = CallasDonnerhackeFinneyShawThayerRFC4880::
				PublicKeyBlockParse(armored_pubkey, opt_verbose, primary);
			if (parse_ok)
			{
				primary->CheckSelfSignatures(ring, opt_verbose);
				if (!primary->valid)
				{
					std::cerr << "WARNING: primary key #" << (j-1) <<
						" is not valid" << std::endl;
					delete primary;
					delete ring;
					continue;
				}
				primary->CheckSubkeys(ring, opt_verbose);
				primary->Reduce(); // keep only valid subkeys
				if (primary->Weak(opt_verbose))
				{
					std::cerr << "WARNING: primary key #" << (j-1) <<
						" is weak" << std::endl;
					delete primary;
					delete ring;
					continue;
				}
			}
			else
			{
				std::cerr << "WARNING: cannot parse primary key #" << (j-1) <<
					std::endl;
				delete ring;
				continue;
			}
			// select corresponding public key of the issuer from subkeys
			bool subkey_selected = false;
			size_t subkey_idx = 0, keyusage = 0;
			time_t ckeytime = 0, ekeytime = 0;
			for (size_t k = 0; k < primary->subkeys.size(); k++)
			{
				if (((primary->subkeys[k]->AccumulateFlags() & 0x02) == 0x02) ||
				    (!primary->subkeys[k]->AccumulateFlags() &&
					((primary->subkeys[k]->pkalgo == TMCG_OPENPGP_PKALGO_RSA) || 
					(primary->subkeys[k]->pkalgo == TMCG_OPENPGP_PKALGO_RSA_SIGN_ONLY) ||
					(primary->subkeys[k]->pkalgo == TMCG_OPENPGP_PKALGO_DSA) ||
					(primary->subkeys[k]->pkalgo == TMCG_OPENPGP_PKALGO_ECDSA))))
				{
					if (CallasDonnerhackeFinneyShawThayerRFC4880::
						OctetsCompare(sigs[i]->issuer, primary->subkeys[k]->id) &&
						!primary->subkeys[k]->Weak(opt_verbose))
					{
						subkey_selected = true;
						subkey_idx = k;
						keyusage = primary->subkeys[k]->AccumulateFlags();
						ckeytime = primary->subkeys[k]->creationtime;
						ekeytime = primary->subkeys[k]->expirationtime;
						break;
					}
				}
			}
			// check the primary key, if no admissible subkey has been selected
			if (!subkey_selected)
			{
				if (((primary->AccumulateFlags() & 0x02) != 0x02) &&
				    (!primary->AccumulateFlags() &&
					(primary->pkalgo != TMCG_OPENPGP_PKALGO_RSA) &&
					(primary->pkalgo != TMCG_OPENPGP_PKALGO_RSA_SIGN_ONLY) &&
					(primary->pkalgo != TMCG_OPENPGP_PKALGO_DSA) &&
					(primary->pkalgo != TMCG_OPENPGP_PKALGO_ECDSA)))
				{
					std::cerr << "WARNING: no admissible public key found" <<
						std::endl;
					delete primary;
					delete ring;
					continue;
				}
				if (!CallasDonnerhackeFinneyShawThayerRFC4880::
					OctetsCompare(sigs[i]->issuer, primary->id))
				{
					std::cerr << "WARNING: no admissible public key found" <<
						std::endl;
					delete primary;
					delete ring;
					continue;
				}
				keyusage = primary->AccumulateFlags();
				ckeytime = primary->creationtime;
				ekeytime = primary->expirationtime;
			}
			// additional validity checks on selected key and signature
			time_t current_time = time(NULL);
			// 1. key validity time (signatures made before key creation or
			//    after key expiry are not valid)
			if (sigs[i]->creationtime < ckeytime)
			{
				delete primary;
				delete ring;
				continue;
			}
			if (ekeytime && (sigs[i]->creationtime > (ckeytime + ekeytime)))
			{
				delete primary;
				delete ring;
				continue;
			}
			// 2. signature validity time (expired signatures are not valid)
			if (sigs[i]->expirationtime &&
				(current_time > (sigs[i]->creationtime + sigs[i]->expirationtime)))
			{
				delete primary;
				delete ring;
				continue;
			}
			// 3. key usage flags (signatures made by keys not with the
			//    "signing" capability are not valid)
			if ((keyusage & 0x02) != 0x02)
			{
				delete primary;
				delete ring;
				continue;
			}
			// 4. key validity time (expired keys are not valid)
			if (ekeytime && (current_time > (ckeytime + ekeytime)))
			{
				delete primary;
				delete ring;
				continue;
			}
			// verify signature cryptographically
			bool verify_ok = false;
			std::string fpr;
			if (subkey_selected)
			{
				TMCG_OpenPGP_Subkey *sub = primary->subkeys[subkey_idx];
				verify_ok = sigs[i]->VerifyData(sub->key, data, opt_verbose);
				CallasDonnerhackeFinneyShawThayerRFC4880::
					FingerprintConvertPlain(sub->fingerprint, fpr);
			}
			else
			{
				verify_ok = sigs[i]->VerifyData(primary->key, data, opt_verbose);
				CallasDonnerhackeFinneyShawThayerRFC4880::
					FingerprintConvertPlain(primary->fingerprint, fpr);
			}
			if (opt_verbose)
			{
				std::cerr << "INFO: key #" << (j-1) << " is " << fpr <<
					std::endl;
			}
			if (verify_ok)
			{
				std::string v;
				struct tm *ut = gmtime(&current_time);
				char buf[1024];
				memset(buf, 0, sizeof(buf));
				strftime(buf, sizeof(buf), "%FT%TZ", ut);
				v += buf; // ISO-8601 UTC datestamp
				v += " ";
				v += fpr; // Fingerprint of the signing key (may be a subkey)
				v += " ";
				fpr = "";
				CallasDonnerhackeFinneyShawThayerRFC4880::
					FingerprintConvertPlain(primary->fingerprint, fpr);
				v += fpr; // Fingerprint of primary key of signing certificate
				v += " ";
				v += args[j]; // message describing the verification (free form)
				verifications.push_back(v);
			}
			delete primary;
			delete ring;
		}
	}
	for (size_t i = 0; i < sigs.size(); i++)
		delete sigs[i];
	return true;
}

bool timestamp
	(const std::string &s, time_t &ts)
{
	if (s == "-")
	{
		ts = 0; // beginning or end of time	
	}
	else if (s == "now")
	{
		ts = time(NULL); // now
	}
	else
	{
		struct tm t = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
		int tz_hour = 0, tz_min = 0, n = 0;
		float sec = 0.0;
		n = sscanf(s.c_str(), "%d-%d-%dT%d:%d:%f%d:%dZ",
				&t.tm_year, &t.tm_mon, &t.tm_mday,
				&t.tm_hour, &t.tm_min, &sec, &tz_hour, &tz_min);
		if ((n < 6) || (n > 8))
			return false;
		if (tz_hour < 0)
			tz_min = -tz_min;
		t.tm_year -= 1900;
		t.tm_mon -= 1;
		t.tm_sec = (int)sec;
		t.tm_hour += tz_hour;
		t.tm_min += tz_min;
		ts = mktime(&t);
		if (ts == ((time_t) -1))
			return false;
	}
	return true;
}

bool encrypt_session_key
	(const TMCG_OpenPGP_Subkey* sub, const tmcg_openpgp_secure_octets_t &seskey,
	 const tmcg_openpgp_octets_t &subkeyid, tmcg_openpgp_octets_t &out)
{
	gcry_error_t ret;
	if ((sub->pkalgo == TMCG_OPENPGP_PKALGO_RSA) ||
	    (sub->pkalgo == TMCG_OPENPGP_PKALGO_RSA_ENCRYPT_ONLY))
	{
		gcry_mpi_t me;
		me = gcry_mpi_new(2048);
		ret = CallasDonnerhackeFinneyShawThayerRFC4880::
			AsymmetricEncryptRSA(seskey, sub->key, me);
		if (ret)
		{
			std::cerr << "ERROR: AsymmetricEncryptRSA() failed" <<
				" (rc = " << gcry_err_code(ret) << ")" <<
				std::endl;
			gcry_mpi_release(me);
			return false;
		}
		CallasDonnerhackeFinneyShawThayerRFC4880::
			PacketPkeskEncode(subkeyid, me, out);
		gcry_mpi_release(me);
	}
	else if (sub->pkalgo == TMCG_OPENPGP_PKALGO_ELGAMAL)
	{	
		// Note that OpenPGP ElGamal encryption in $Z^*_p$ provides only
		// OW-CPA security under the CDH assumption. In order to achieve
		// at least IND-CPA (aka semantic) security under DDH assumption
		// the encoded message $m$ must be an element of the prime-order
		// subgroup $G_q$ generated by $g$ (algebraic structure of tElG).
		// Unfortunately, the probability that this happens is negligible,
		// if the size of prime $q$ is much smaller than the size of $p$.
		// We cannot enforce $m\in G_q$ since $m$ is padded according to
		// OpenPGP (PKCS#1). Thus, one bit of the session key is leaked.
		gcry_mpi_t gk, myk;
		gk = gcry_mpi_new(2048);
		myk = gcry_mpi_new(2048);
		ret = CallasDonnerhackeFinneyShawThayerRFC4880::
			AsymmetricEncryptElgamal(seskey, sub->key, gk, myk);
		if (ret)
		{
			std::cerr << "ERROR: AsymmetricEncryptElgamal() failed" <<
				" (rc = " << gcry_err_code(ret) << ")" <<
				std::endl;
			gcry_mpi_release(gk);
			gcry_mpi_release(myk);
			return false;
		}
		CallasDonnerhackeFinneyShawThayerRFC4880::
			PacketPkeskEncode(subkeyid, gk, myk, out);
		gcry_mpi_release(gk);
		gcry_mpi_release(myk);
	}
	else if (sub->pkalgo == TMCG_OPENPGP_PKALGO_ECDH)
	{
		gcry_mpi_t ecepk;
		size_t rkwlen = 0;
		tmcg_openpgp_byte_t rkw[256];
		ecepk = gcry_mpi_new(1024);
		memset(rkw, 0, sizeof(rkw));
		ret = CallasDonnerhackeFinneyShawThayerRFC4880::
			AsymmetricEncryptECDH(seskey, sub->key,
				sub->kdf_hashalgo, sub->kdf_skalgo,
				sub->ec_curve, sub->fingerprint, ecepk,
				rkwlen, rkw);
		if (ret)
		{
			std::cerr << "ERROR: AsymmetricEncryptECDH() failed" <<
				" (rc = " << gcry_err_code(ret) << ")" <<
				std::endl;
			gcry_mpi_release(ecepk);
			return false;
		}
		CallasDonnerhackeFinneyShawThayerRFC4880::
			PacketPkeskEncode(subkeyid, ecepk, rkwlen, rkw, out);
		gcry_mpi_release(ecepk);
	}
	else
	{
		std::cerr << "ERROR: public-key algorithm " <<
			(int)sub->pkalgo << " not supported" << std::endl;
		return false;
	}
	return true;
}

bool encrypt_session_key
	(const TMCG_OpenPGP_Pubkey* pub, const tmcg_openpgp_secure_octets_t &seskey,
	 const tmcg_openpgp_octets_t &keyid, tmcg_openpgp_octets_t &out)
{
	bool ret;
	if (pub->pkalgo != TMCG_OPENPGP_PKALGO_RSA)
	{
		return false; // only RSA is an encryption-capable primary key algo
	}
	TMCG_OpenPGP_Subkey *sub = new TMCG_OpenPGP_Subkey(pub->pkalgo,
		pub->creationtime, pub->expirationtime, pub->rsa_n, pub->rsa_e,
		pub->packet);
	ret = encrypt_session_key(sub, seskey, keyid, out);
	delete sub;
	return ret;
}

gcry_error_t encrypt_kek
	(const tmcg_openpgp_octets_t &kek, const tmcg_openpgp_skalgo_t algo,
	 const tmcg_openpgp_secure_octets_t &key, tmcg_openpgp_octets_t &out)
{
	gcry_error_t ret = 0;
	size_t bs = CallasDonnerhackeFinneyShawThayerRFC4880::
		AlgorithmIVLength(algo); // get block size of algorithm
	size_t ks = CallasDonnerhackeFinneyShawThayerRFC4880::
		AlgorithmKeyLength(algo); // get key size of algorithm
	if ((bs == 0) || (ks == 0))
		return gcry_error(GPG_ERR_CIPHER_ALGO); // error: bad algorithm
	size_t buflen = (kek.size() >= key.size()) ? kek.size() : key.size();
	unsigned char *buf = (unsigned char*)gcry_malloc_secure(buflen);
	if (buf == NULL)
		return gcry_error(GPG_ERR_RESOURCE_LIMIT); // cannot alloc secure memory
	gcry_cipher_hd_t hd;
	ret = gcry_cipher_open(&hd, CallasDonnerhackeFinneyShawThayerRFC4880::
		AlgorithmSymGCRY(algo), GCRY_CIPHER_MODE_CFB, GCRY_CIPHER_SECURE);
	if (ret)
	{
		gcry_free(buf);
		return ret;
	}
	for (size_t i = 0; i < key.size(); i++)
		buf[i] = key[i];
	ret = gcry_cipher_setkey(hd, buf, key.size());
	if (ret)
	{
		gcry_free(buf);
		gcry_cipher_close(hd);
		return ret;
	}
	// set "an IV of all zeros" [RFC 4880]
	ret = gcry_cipher_setiv(hd, NULL, 0);
	if (ret)
	{
		gcry_free(buf);
		gcry_cipher_close(hd);
		return ret;
	}
	for (size_t i = 0; i < kek.size(); i++)
		buf[i] = kek[i];
	ret = gcry_cipher_encrypt(hd, buf, kek.size(), NULL, 0);
	if (ret)
	{
		gcry_free(buf);
		gcry_cipher_close(hd);
		return ret;
	}
	for (size_t i = 0; i < kek.size(); i++)
		out.push_back(buf[i]);
	gcry_free(buf);
	gcry_cipher_close(hd);
	return ret;
}

bool encrypt
	(const std::vector<std::string> &args,
	 const tmcg_openpgp_secure_string_t &passphrase,
	 const tmcg_openpgp_octets_t &data)
{
	tmcg_openpgp_octets_t lit, prefix, enc;
	tmcg_openpgp_secure_octets_t seskey;
	tmcg_openpgp_skalgo_t skalgo = TMCG_OPENPGP_SKALGO_AES256; // fixed alg
	CallasDonnerhackeFinneyShawThayerRFC4880::PacketTagEncode(11, lit);
	CallasDonnerhackeFinneyShawThayerRFC4880::
		PacketLengthEncode(1+1+4+data.size(), lit);
	if (opt_as_text)
		lit.push_back(0x75); // format: text data (utf-8)
	else if (opt_as_mime)
		lit.push_back(0x6d); // format: MIME data
	else
		lit.push_back(0x62); // format: binary data
	lit.push_back(0); // no file name
	CallasDonnerhackeFinneyShawThayerRFC4880::PacketTimeEncode(lit); // time
	lit.insert(lit.end(), data.begin(), data.end()); // data
	tmcg_openpgp_octets_t sigs;
	for (size_t i = 0; i < opt_sign_with.size(); i++)
	{
		std::string armored_key;
		if (!autodetect_file(opt_sign_with[i],
			TMCG_OPENPGP_ARMOR_PRIVATE_KEY_BLOCK, armored_key))
		{
			encrypt_ret = 61; // "MISSING_INPUT" [DKG20]
			return false;
		}
		TMCG_OpenPGP_Prvkey *prv = NULL;
		if (!CallasDonnerhackeFinneyShawThayerRFC4880::
			PrivateKeyBlockParse(armored_key, opt_verbose, passphrase, prv))
		{
			encrypt_ret = 67; // "KEY_IS_PROTECTED" [DKG20]
			return false;
		}
		time_t sigtime = time(NULL); // current time, fixed hash algo SHA2-512
		tmcg_openpgp_hashalgo_t hashalgo = TMCG_OPENPGP_HASHALGO_SHA512;
		tmcg_openpgp_octets_t trailer, hash, left;
		bool hret = false;
		if (opt_as_text || (opt_as_mime && valid_utf8(data)))
		{
			CallasDonnerhackeFinneyShawThayerRFC4880::
				PacketSigPrepareDetachedSignature(
					TMCG_OPENPGP_SIGNATURE_CANONICAL_TEXT_DOCUMENT,
					prv->pub->pkalgo, hashalgo, sigtime, 0, "",
					prv->pub->fingerprint, trailer);
			hret = CallasDonnerhackeFinneyShawThayerRFC4880::
				TextDocumentHash(data, trailer, hashalgo, hash, left);
		}
		else
		{
			CallasDonnerhackeFinneyShawThayerRFC4880::
				PacketSigPrepareDetachedSignature(
					TMCG_OPENPGP_SIGNATURE_BINARY_DOCUMENT, prv->pub->pkalgo,
					hashalgo, sigtime, 0, "", prv->pub->fingerprint, trailer);
			hret = CallasDonnerhackeFinneyShawThayerRFC4880::
				BinaryDocumentHash(data, trailer, hashalgo, hash, left);
		}
		if (!hret)
		{
			std::cerr << "ERROR: [Text|Binary]DocumentHash() failed" <<
				std::endl;
			delete prv;
			encrypt_ret = -1;
			return false;
		}
		gcry_error_t ret;
		gcry_mpi_t r, s;
		r = gcry_mpi_new(2048);
		s = gcry_mpi_new(2048);
		switch (prv->pkalgo)
		{
			case TMCG_OPENPGP_PKALGO_RSA:
			case TMCG_OPENPGP_PKALGO_RSA_SIGN_ONLY:
				ret = CallasDonnerhackeFinneyShawThayerRFC4880::
					AsymmetricSignRSA(hash, prv->private_key, hashalgo, s);
				break;
			case TMCG_OPENPGP_PKALGO_DSA:
				ret = CallasDonnerhackeFinneyShawThayerRFC4880::
					AsymmetricSignDSA(hash, prv->private_key, r, s);
				break;
			case TMCG_OPENPGP_PKALGO_ECDSA:
				ret = CallasDonnerhackeFinneyShawThayerRFC4880::
					AsymmetricSignECDSA(hash, prv->private_key, r, s);
				break;
			case TMCG_OPENPGP_PKALGO_EDDSA:
				ret = CallasDonnerhackeFinneyShawThayerRFC4880::
					AsymmetricSignEdDSA(hash, prv->private_key, r, s);
				break;
			default:
				std::cerr << "ERROR: public-key algorithm " <<
					(int)prv->pkalgo << " not supported" << std::endl;
				gcry_mpi_release(r), gcry_mpi_release(s);
				delete prv;
				encrypt_ret = 13; // "UNSUPPORTED_ASYMMETRIC_ALGO" [DKG20]
				return false;
		}
		if (ret)
		{
			std::cerr << "ERROR: signing of hash value failed " <<
				"(rc = " << gcry_err_code(ret) << ", str = " <<
				gcry_strerror(ret) << ")" << std::endl;
			gcry_mpi_release(r), gcry_mpi_release(s);
			delete prv;
			encrypt_ret = -1;
			return false;
		}
		switch (prv->pkalgo)
		{
			case TMCG_OPENPGP_PKALGO_RSA:
			case TMCG_OPENPGP_PKALGO_RSA_SIGN_ONLY:
				CallasDonnerhackeFinneyShawThayerRFC4880::
					PacketSigEncode(trailer, left, s, sigs);
				break;
			case TMCG_OPENPGP_PKALGO_DSA:
			case TMCG_OPENPGP_PKALGO_ECDSA:
			case TMCG_OPENPGP_PKALGO_EDDSA:
				CallasDonnerhackeFinneyShawThayerRFC4880::
					PacketSigEncode(trailer, left, r, s, sigs);
				break;
			default:
				std::cerr << "ERROR: public-key algorithm " <<
					(int)prv->pkalgo << " not supported" << std::endl;
				gcry_mpi_release(r), gcry_mpi_release(s);
				delete prv;
				encrypt_ret = 13; // "UNSUPPORTED_ASYMMETRIC_ALGO" [DKG20]
				return false;
		}
		gcry_mpi_release(r), gcry_mpi_release(s);
		delete prv;
	}
	lit.insert(lit.begin(), sigs.begin(), sigs.end()); // prepend signatures
	gcry_error_t ret = CallasDonnerhackeFinneyShawThayerRFC4880::
		SymmetricEncryptAES256(lit, seskey, prefix, true, enc); // encrypt (1)
	if (ret)
	{
		std::cerr << "ERROR: SymmetricEncryptAES256() failed (rc = " <<
			gcry_err_code(ret) << ")" << std::endl;
		encrypt_ret = -1;
		return false;
	}
	tmcg_openpgp_octets_t mdc_hashing, hash, mdc, seipd;
	enc.clear();
	// "it includes the prefix data described above" [RFC 4880]
	mdc_hashing.insert(mdc_hashing.end(), prefix.begin(), prefix.end());
	// "it includes all of the plaintext" [RFC 4880]
	mdc_hashing.insert(mdc_hashing.end(), lit.begin(), lit.end());
	// "and the also includes two octets of values 0xD3, 0x14" [RFC 4880]
	mdc_hashing.push_back(0xD3);
	mdc_hashing.push_back(0x14);
	hash.clear();
	// "passed through the SHA-1 hash function" [RFC 4880]
	CallasDonnerhackeFinneyShawThayerRFC4880::
		HashCompute(TMCG_OPENPGP_HASHALGO_SHA1, mdc_hashing, hash);
	CallasDonnerhackeFinneyShawThayerRFC4880::PacketMdcEncode(hash, mdc);
	lit.insert(lit.end(), mdc.begin(), mdc.end()); // append MDC packet
	// generate a fresh session key, but keep the previous prefix
	seskey.clear();
	ret = CallasDonnerhackeFinneyShawThayerRFC4880::
		SymmetricEncryptAES256(lit, seskey, prefix, false, enc); // encrypt (2)
	if (ret)
	{
		std::cerr << "ERROR: SymmetricEncryptAES256() failed (rc = " <<
			gcry_err_code(ret) << ")" << std::endl;
		encrypt_ret = -1;
		return false;
	}
	CallasDonnerhackeFinneyShawThayerRFC4880::PacketSeipdEncode(enc, seipd);
	// additionally, encrypt the message with appropriate AEAD algorithm
	tmcg_openpgp_octets_t aead;
	tmcg_openpgp_aeadalgo_t aeadalgo = TMCG_OPENPGP_AEADALGO_OCB; // default
#if GCRYPT_VERSION_NUMBER < 0x010900
	// FIXME: remove, if libgcrypt >= 1.9.0 required by configure.ac
#else
	aeadalgo = TMCG_OPENPGP_AEADALGO_EAX;
#endif
	if (opt_rfc4880bis)
	{
		enc.clear();
		tmcg_openpgp_octets_t ad, iv;
		tmcg_openpgp_byte_t cs = 10; // fixed chunk of size 2^16 bytes
		ad.push_back(0xD4); // packet tag in new format
		ad.push_back(0x01); // packet version number
		ad.push_back(skalgo); // cipher algorithm octet
		ad.push_back(aeadalgo); // AEAD algorithm octet
		ad.push_back(cs); // chunk size octet
		for (size_t i = 0; i < 8; i++)
			ad.push_back(0x00); // initial eight-octet big-endian chunk index
		ret = CallasDonnerhackeFinneyShawThayerRFC4880::
			SymmetricEncryptAEAD(lit, seskey, skalgo, aeadalgo, cs, ad,
				opt_verbose, iv, enc); // encrypt (3)
		if (ret)
		{
			std::cerr << "ERROR: SymmetricEncryptAEAD() failed (rc = " <<
				gcry_err_code(ret) << ")" << std::endl;
			encrypt_ret = -1;
			return false;
		}
		CallasDonnerhackeFinneyShawThayerRFC4880::
			PacketAeadEncode(skalgo, aeadalgo, cs, iv, enc, aead);
	}
	// perform password-based symmetric encryption, for all given passwords
	tmcg_openpgp_octets_t msg;
	bool some_skesk = false;
	for (size_t k = 0; k < opt_with_password.size(); k++)
	{
		tmcg_openpgp_secure_string_t p;
		for (size_t i = 0; i < opt_with_password[k].length(); i++)
			p += opt_with_password[k][i];
		// encrypt session key with passphrase according to S2K
		tmcg_openpgp_octets_t plain, salt, iv2, es;
		tmcg_openpgp_hashalgo_t s2k_hashalgo = TMCG_OPENPGP_HASHALGO_SHA512;
		tmcg_openpgp_byte_t rand[8], count = 0xFD; // set resonable S2K count
		tmcg_openpgp_secure_octets_t kek;
		gcry_randomize(rand, sizeof(rand), GCRY_STRONG_RANDOM);
		for (size_t i = 0; i < sizeof(rand); i++)
			salt.push_back(rand[i]);
		CallasDonnerhackeFinneyShawThayerRFC4880::
			S2KCompute(s2k_hashalgo, 32, p, salt, true, count, kek);
		if (opt_verbose > 2)
		{
			std::cerr << "INFO: kek.size() = " << kek.size() << std::endl;
			std::cerr << "INFO: seskey.size() = " << seskey.size() << std::endl;
			std::cerr << "INFO: seskey[0] = " << (int)seskey[0] << std::endl;
		}
		if ((seskey.size() == 35) && (kek.size() == 32))
		{
			// strip the always appended checksum from session key
			plain.insert(plain.end(), seskey.begin(), seskey.end()-2);
		}
		else
			std::cerr << "ERROR: bad session key for SKESK" << std::endl;
		ret = encrypt_kek(plain, skalgo, kek, es);
		if (ret)
		{
			encrypt_ret = -1;
			return false;
		}
		if (opt_verbose > 2)
		{
			std::cerr << "INFO: es.size() = " << es.size() << std::endl;
			std::cerr << "INFO: iv2.size() = " << iv2.size() << std::endl;
		}
		// create a corresponding SKESK packet
		CallasDonnerhackeFinneyShawThayerRFC4880::PacketTagEncode(3, msg);
		CallasDonnerhackeFinneyShawThayerRFC4880::
			PacketLengthEncode(5+salt.size()+es.size(), msg);
		msg.push_back(4); // V4 format
		msg.push_back(skalgo);
		msg.push_back(TMCG_OPENPGP_STRINGTOKEY_ITERATED); // Iterated+Salted
		msg.push_back(s2k_hashalgo); // S2K hash algo
		msg.insert(msg.end(), salt.begin(), salt.end()); // salt
		msg.push_back(count); // count, a one-octet, coded value
		msg.insert(msg.end(), es.begin(), es.end()); // encrypted session key
		some_skesk = true;
	}
	// perform public-key encryption, for all specified encryption keys
	size_t features = 0xFF;
	bool some_pkesk = false;
	for (size_t k = 0; k < args.size(); k++)
	{
		std::string armored_pubkey;
		if (!autodetect_file(args[k],
			TMCG_OPENPGP_ARMOR_PUBLIC_KEY_BLOCK, armored_pubkey))
		{
			encrypt_ret = 61; // "MISSING_INPUT" [DKG20]
			return false;
		}
		// parse the public key block and check self-signatures
		TMCG_OpenPGP_Keyring *ring = new TMCG_OpenPGP_Keyring();
		TMCG_OpenPGP_Pubkey *primary = NULL;
		bool parse_ok = CallasDonnerhackeFinneyShawThayerRFC4880::
			PublicKeyBlockParse(armored_pubkey, opt_verbose, primary);
		if (!parse_ok)
		{
			std::cerr << "ERROR: cannot parse public key #" << k << std::endl;
			delete ring;
			encrypt_ret = 17; // "CERT_CANNOT_ENCRYPT" [DKG20]
			return false;
		}
		primary->CheckSelfSignatures(ring, opt_verbose);
		if (!primary->valid)
		{
			std::cerr << "ERROR: primary key #" << k << " is invalid" <<
				std::endl;
			delete primary;
			delete ring;
			encrypt_ret = 17; // "CERT_CANNOT_ENCRYPT" [DKG20]
			return false;
		}
		primary->CheckSubkeys(ring, opt_verbose);
		primary->Reduce(); // keep only valid subkeys
		if (primary->Weak(opt_verbose))
		{
			std::cerr << "ERROR: primary key #" << k << " is weak" << std::endl;
			delete primary;
			delete ring;
			encrypt_ret = 17; // "CERT_CANNOT_ENCRYPT" [DKG20]
			return false;
		}
		// select encryption-capable subkeys
		std::vector<TMCG_OpenPGP_Subkey*> selected;
		for (size_t j = 0; j < primary->subkeys.size(); j++)
		{
			// encryption-capable subkey?
			if (((primary->subkeys[j]->AccumulateFlags() & 0x04) == 0x04) ||
			    ((primary->subkeys[j]->AccumulateFlags() & 0x08) == 0x08) ||
			    (!primary->subkeys[j]->AccumulateFlags() &&
					((primary->subkeys[j]->pkalgo == TMCG_OPENPGP_PKALGO_RSA) || 
					(primary->subkeys[j]->pkalgo == TMCG_OPENPGP_PKALGO_RSA_ENCRYPT_ONLY) ||
					(primary->subkeys[j]->pkalgo == TMCG_OPENPGP_PKALGO_ELGAMAL) ||
					(primary->subkeys[j]->pkalgo == TMCG_OPENPGP_PKALGO_ECDH))))
			{
				if (primary->subkeys[j]->Weak(opt_verbose))
				{
					std::cerr << "WARNING: weak subkey #" <<
						j << " of public key #" << k <<
						" ignored" << std::endl;
					continue;
				}
				if ((primary->subkeys[j]->pkalgo != TMCG_OPENPGP_PKALGO_RSA) &&
				         (primary->subkeys[j]->pkalgo != TMCG_OPENPGP_PKALGO_RSA_ENCRYPT_ONLY) &&
				         (primary->subkeys[j]->pkalgo != TMCG_OPENPGP_PKALGO_ELGAMAL) &&
					 (primary->subkeys[j]->pkalgo != TMCG_OPENPGP_PKALGO_ECDH))
				{
					std::cerr << "WARNING: subkey #" << j <<
						" of public key #" << k <<
						" with unsupported public-key" <<
						" algorithm ignored" << std::endl;
					continue; // FIXME: ret = 13 UNSUPPORTED_ASYMMETRIC_ALGO
				}
				selected.push_back(primary->subkeys[j]);
				size_t skf = primary->subkeys[j]->AccumulateFeatures();
				size_t pkf = primary->AccumulateFeatures();
				features &= (skf | pkf);
				if ((std::find(primary->subkeys[j]->psa.begin(),
					primary->subkeys[j]->psa.end(),
					skalgo) == primary->subkeys[j]->psa.end()) &&
				    (std::find(primary->psa.begin(), primary->psa.end(),
						skalgo) == primary->psa.end()))
				{
					if (opt_verbose)
					{
						std::cerr << "WARNING: AES-256 is none of the" <<
							" preferred symmetric algorithms;" <<
							" use AES-256 anyway" << std::endl;
					}
				}
				if (((skf & 0x01) != 0x01) &&
				    ((pkf & 0x01) != 0x01))
				{
					if (opt_verbose)
					{
						std::cerr << "WARNING: recipient does not state" <<
							" support for modification detection (MDC);" <<
							"use MDC anyway" << std::endl;
					}
				}
				if (opt_rfc4880bis)
				{
					if (((skf & 0x02) != 0x02) &&
					    ((pkf & 0x02) != 0x02))
					{
						if (opt_verbose)
						{
							std::cerr << "WARNING: recipient does not state" <<
								" support for AEAD Encrypted Data Packet;" <<
								" AEAD disabled" << std::endl;
						}
					}
					if ((std::find(primary->subkeys[j]->paa.begin(),
						primary->subkeys[j]->paa.end(),
						aeadalgo) == primary->subkeys[j]->paa.end()) &&
					    (std::find(primary->paa.begin(), primary->paa.end(),
							aeadalgo) == primary->paa.end()))
					{
						if (opt_verbose)
						{
							std::cerr << "WARNING: selected algorithm is" <<
								" none of the preferred AEAD algorithms;" <<
								" AEAD disabled" << std::endl;
						}
						aead.clear(); // fallback to SEIPD packet
					}
				}
			}
		}
		// check primary key, if no encryption-capable subkeys selected
		if ((selected.size() == 0) && primary->valid)
		{
			// encryption-capable key or RSA key without flags?
			if (((primary->AccumulateFlags() & 0x04) == 0x04) ||
			    ((primary->AccumulateFlags() & 0x08) == 0x08))
			{
				if (opt_verbose)
				{
					std::cerr << "INFO: primary key #" << k << " is" <<
						" encryption-capable and will be used" << std::endl;
				}
			}
			else if ((primary->AccumulateFlags() == 0) &&
					((primary->pkalgo == TMCG_OPENPGP_PKALGO_RSA) ||
					(primary->pkalgo == TMCG_OPENPGP_PKALGO_RSA_ENCRYPT_ONLY)))
			{
				if (opt_verbose)
				{
					std::cerr << "WARNING: primary key #" << k << " of type" <<
						" RSA without key flags found and used" << std::endl;
				}
			}
			else
			{
				std::cerr << "ERROR: no encryption-capable public key" <<
					" found for key #" << k << std::endl;
				delete primary;
				delete ring;
				encrypt_ret = 17; // "CERT_CANNOT_ENCRYPT" [DKG20]
				return false;
			}
			size_t pkf = primary->AccumulateFeatures();
			features &= pkf;
			if (std::find(primary->psa.begin(), primary->psa.end(),
				skalgo) == primary->psa.end())
			{
				if (opt_verbose)
				{
					std::cerr << "WARNING: AES-256 is none of the preferred" <<
						" symmetric algorithms; use AES-256 anyway" <<
						std::endl;
				}
			}
			if (((pkf & 0x01) != 0x01))
			{
				if (opt_verbose)
				{
					std::cerr << "WARNING: recipient does not state support" <<
						" for modification detection (MDC);" <<
						"use MDC protection anyway" << std::endl;
				}
			}
			if (opt_rfc4880bis)
			{
				if (((pkf & 0x02) != 0x02))
				{
					if (opt_verbose)
					{
						std::cerr << "WARNING: recipient does not state" <<
							" support for AEAD Encrypted Data Packet; AEAD" <<
							" disabled" << std::endl;
					}
				}
				if (std::find(primary->paa.begin(), primary->paa.end(),
					aeadalgo) == primary->paa.end())
				{
					if (opt_verbose)
					{
						std::cerr << "WARNING: selected algorithm is none of" <<
							" the preferred AEAD algorithms; AEAD disabled" <<
							std::endl;
					}
					aead.clear(); // fallback to SEIPD packet
				}
			}
		}
		// encrypt the session key (create PKESK packet)
		if (opt_verbose > 1)
		{
			std::cerr << "INFO: " << selected.size() << " subkey(s) selected" <<
				" for encryption of session key" << std::endl;
		}
		for (size_t j = 0; j < selected.size(); j++)
		{
			tmcg_openpgp_octets_t pkesk, subkeyid;
			subkeyid.insert(subkeyid.end(),
				selected[j]->id.begin(), selected[j]->id.end());
			if (!encrypt_session_key(selected[j], seskey, subkeyid, pkesk))
			{
				delete primary;
				delete ring;
				std::cerr << "ERROR: encryption failed" << std::endl;
				encrypt_ret = 17; // "CERT_CANNOT_ENCRYPT" [DKG20]
				return false;
			}
			msg.insert(msg.end(), pkesk.begin(), pkesk.end());
			some_pkesk = true;
		}
		if (selected.size() == 0)
		{
			tmcg_openpgp_octets_t pkesk, keyid;
			keyid.insert(keyid.end(),
				primary->id.begin(), primary->id.end());
			if (!encrypt_session_key(primary, seskey, keyid, pkesk))
			{
				delete primary;
				delete ring;
				std::cerr << "ERROR: encryption failed" << std::endl;
				encrypt_ret = 17; // "CERT_CANNOT_ENCRYPT" [DKG20]
				return false;
			}
			msg.insert(msg.end(), pkesk.begin(), pkesk.end());
			some_pkesk = true;
		}
		delete primary;
		delete ring;
	}
	if (!some_pkesk && !some_skesk)
	{
		std::cerr << "ERROR: encryption failed" << std::endl;
		encrypt_ret = 17; // "CERT_CANNOT_ENCRYPT" [DKG20]
		return false;
	}
	// append the encrypted data packet(s) according to supported features
	if (((features & 0x02) == 0x02) && (aead.size() > 0) && (args.size() > 0))
	{
		// append AEAD, because all selected recipients/keys have support
		msg.insert(msg.end(), aead.begin(), aead.end());
	}
	else
	{
		// append SEIPD, because some of the recipients/keys have no AEAD yet
		msg.insert(msg.end(), seipd.begin(), seipd.end());
	}
	// output the result
	if (opt_armor)
	{
		std::string armor;
		CallasDonnerhackeFinneyShawThayerRFC4880::
			ArmorEncode(TMCG_OPENPGP_ARMOR_MESSAGE, msg, armor);
		std::cout << armor << std::endl;
	}
	else
	{
		for (size_t i = 0; i < msg.size(); i++)
			std::cout << msg[i];
	}
	return true;
}

gcry_error_t decrypt_kek
	(const tmcg_openpgp_octets_t &kek, const tmcg_openpgp_skalgo_t algo,
	 const tmcg_openpgp_secure_octets_t &key, tmcg_openpgp_secure_octets_t &out)
{
	gcry_error_t ret = 0;
	size_t bs = CallasDonnerhackeFinneyShawThayerRFC4880::
		AlgorithmIVLength(algo); // get block size of algorithm
	size_t ks = CallasDonnerhackeFinneyShawThayerRFC4880::
		AlgorithmKeyLength(algo); // get key size of algorithm
	if ((bs == 0) || (ks == 0))
		return gcry_error(GPG_ERR_CIPHER_ALGO); // error: bad algorithm
	size_t buflen = (kek.size() >= key.size()) ? kek.size() : key.size();
	unsigned char *buf = (unsigned char*)gcry_malloc_secure(buflen);
	if (buf == NULL)
		return gcry_error(GPG_ERR_RESOURCE_LIMIT); // cannot alloc secure memory
	gcry_cipher_hd_t hd;
	ret = gcry_cipher_open(&hd, CallasDonnerhackeFinneyShawThayerRFC4880::
		AlgorithmSymGCRY(algo), GCRY_CIPHER_MODE_CFB, GCRY_CIPHER_SECURE);
	if (ret)
	{
		gcry_free(buf);
		return ret;
	}
	for (size_t i = 0; i < key.size(); i++)
		buf[i] = key[i];
	ret = gcry_cipher_setkey(hd, buf, key.size());
	if (ret)
	{
		gcry_free(buf);
		gcry_cipher_close(hd);
		return ret;
	}
	// set "an IV of all zeros" [RFC 4880]
	ret = gcry_cipher_setiv(hd, NULL, 0);
	if (ret)
	{
		gcry_free(buf);
		gcry_cipher_close(hd);
		return ret;
	}
	for (size_t i = 0; i < kek.size(); i++)
		buf[i] = kek[i];
	ret = gcry_cipher_decrypt(hd, buf, kek.size(), NULL, 0);
	if (ret)
	{
		gcry_free(buf);
		gcry_cipher_close(hd);
		return ret;
	}
	for (size_t i = 0; i < kek.size(); i++)
		out.push_back(buf[i]);
	gcry_free(buf);
	gcry_cipher_close(hd);
	return ret;
}

bool decrypt_session_key
	(const TMCG_OpenPGP_Message* msg, tmcg_openpgp_secure_octets_t &seskey)
{
	if (msg->SKESKs.size() == 0)
		return false;
	if (opt_verbose > 1)
	{
		std::cerr << "INFO: every PKESK decryption failed;" <<
			" now try each SKESK" << std::endl;
	}
	for (size_t k = 0; k < opt_with_password.size(); k++)
	{
		tmcg_openpgp_secure_string_t p;
		for (size_t i = 0; i < opt_with_password[k].length(); i++)
			p += opt_with_password[k][i];
		for (size_t i = 0; i < msg->SKESKs.size(); i++)
		{
			const TMCG_OpenPGP_SKESK *esk = msg->SKESKs[i];
			tmcg_openpgp_secure_octets_t kek;
			size_t klen = CallasDonnerhackeFinneyShawThayerRFC4880::
				AlgorithmKeyLength(esk->skalgo);
			if (opt_verbose > 2)
			{
				std::cerr << "INFO: s2k_salt = " << std::hex;
				for (size_t j = 0; j < (esk->s2k_salt).size(); j++)
					std::cerr << (int)esk->s2k_salt[j] << " ";
				std::cerr << std::dec << std::endl;
			}
			switch (esk->s2k_type)
			{
				case TMCG_OPENPGP_STRINGTOKEY_SIMPLE:
					if (opt_verbose)
					{
						std::cerr << "WARNING: S2K specifier not" <<
							" supported; skip SKESK" << std::endl;
					}
					break;
				case TMCG_OPENPGP_STRINGTOKEY_SALTED:
					CallasDonnerhackeFinneyShawThayerRFC4880::
						S2KCompute(esk->s2k_hashalgo, klen, p,
							esk->s2k_salt, false, esk->s2k_count, kek);
					break;
				case TMCG_OPENPGP_STRINGTOKEY_ITERATED:
					CallasDonnerhackeFinneyShawThayerRFC4880::
						S2KCompute(esk->s2k_hashalgo, klen, p,
							esk->s2k_salt, true, esk->s2k_count, kek);
					break;
				default:
					if (opt_verbose)
					{
						std::cerr << "WARNING: S2K specifier not" <<
							" supported; skip SKESK" << std::endl;
					}
					break;
			}
			if (opt_verbose > 2)
			{
				std::cerr << "INFO: kek.size() = " << kek.size() << std::endl;
				std::cerr << "INFO: kek = " << std::hex;
				for (size_t j = 0; j < kek.size(); j++)
					std::cerr << (int)kek[j] << " ";
				std::cerr << std::dec << std::endl;
			}
			seskey.clear();
			if (esk->encrypted_key.size() > 0)
			{
				gcry_error_t ret = 0;
				if (esk->aeadalgo == 0)
				{
					ret = decrypt_kek(esk->encrypted_key, esk->skalgo, kek,
						seskey);
				}
				else
				{
					tmcg_openpgp_octets_t decrypted_key;
					tmcg_openpgp_octets_t ad; // additional data
					ad.push_back(0xC3); // packet tag in new format
					ad.push_back(esk->version); // packet version number
					ad.push_back(esk->skalgo); // cipher algorithm octet
					ad.push_back(esk->aeadalgo); // AEAD algorithm octet
					ret = CallasDonnerhackeFinneyShawThayerRFC4880::
						SymmetricDecryptAEAD(esk->encrypted_key, kek,
							esk->skalgo, esk->aeadalgo, 0, esk->iv, ad,
							opt_verbose, decrypted_key);
					for (size_t j = 0; j < decrypted_key.size(); j++)
						seskey.push_back(decrypted_key[j]);
				}
				if (ret)
				{
					std::cerr << "ERROR: SymmetricDecrypt[AEAD]() failed" <<
						" with rc = " << gcry_err_code(ret) <<
						" str = " << gcry_strerror(ret) << std::endl;
					return false;
				}
			}
			else
			{
				seskey.push_back(esk->skalgo);
				for (size_t j = 0; j < kek.size(); j++)
					seskey.push_back(kek[j]);
			}
			// quick check, whether decryption of session key was successful
			tmcg_openpgp_octets_t tmpmsg;				
			if (msg->Decrypt(seskey, 0, tmpmsg))
				return true;
		}
	}
	return false;
}

bool decompress_libz
	(const TMCG_OpenPGP_Message* msg, tmcg_openpgp_octets_t &infmsg)
{
	int rc = 0;
	z_stream zs;
	unsigned char zin[4096];
	unsigned char zout[4096];
	zs.zalloc = Z_NULL;
	zs.zfree = Z_NULL;
	zs.opaque = Z_NULL;
	zs.avail_in = 0;
	zs.next_in = Z_NULL;
	static const char* myZlibVersion = ZLIB_VERSION;
	if (zlibVersion()[0] != myZlibVersion[0])
	{
		if (opt_verbose > 1)
			std::cerr << "ERROR: incompatible zlib versions found" << std::endl;
		return false;
	}
	else if (std::strcmp(zlibVersion(), ZLIB_VERSION) != 0)
	{
		if (opt_verbose > 1)
			std::cerr << "WARNING: different zlib versions found" << std::endl;
	}
	switch (msg->compalgo)
	{
		case TMCG_OPENPGP_COMPALGO_ZIP:
			rc = inflateInit2(&zs, -15);
			break;
		case TMCG_OPENPGP_COMPALGO_ZLIB:
			rc = inflateInit(&zs);
			break;
		default:
			if (opt_verbose)
			{
				std::cerr << "ERROR: compression algorithm " <<
					(int)msg->compalgo << " is not supported" << std::endl;
			}
			return false;
			break;
	}
	if (rc != Z_OK)
	{
		if (opt_verbose)
		{
			std::cerr << "ZLIB ERROR: " << (int)rc;
			if (zs.msg != NULL)
				std::cerr << " " << zs.msg;
			std::cerr << std::endl;
		}
		return false;
	}
	size_t cnt = 0;
	memset(zin, 0, sizeof(zin));
	do
	{
		if (zs.avail_in == 0)
		{
			size_t zlen = 0;
			for (size_t i = 0; i < sizeof(zin); i++)
			{
				if (cnt >= (msg->compressed_data).size())
					break;
				zin[i] = (msg->compressed_data)[cnt];
				zlen++, cnt++;
			}
			zs.avail_in = zlen;
			zs.next_in = zin;
		}
		memset(zout, 0, sizeof(zout));
		zs.avail_out = sizeof(zout);
		zs.next_out = zout;
		rc = inflate(&zs, Z_SYNC_FLUSH);
		if ((rc == Z_NEED_DICT) || (rc == Z_DATA_ERROR) ||
			(rc == Z_MEM_ERROR) || (rc == Z_STREAM_ERROR))
		{
			if (opt_verbose)
			{
				std::cerr << "ZLIB ERROR: " << rc;
				if (zs.msg != NULL)
					std::cerr << " " << zs.msg;
				std::cerr << std::endl;
			}
			(void)inflateEnd(&zs);
			return false;
		}
		for (size_t i = 0; i < (sizeof(zout) - zs.avail_out); i++)
			infmsg.push_back(zout[i]);
	}
	while ((rc != Z_STREAM_END) && (rc != Z_BUF_ERROR));
	(void)inflateEnd(&zs);
	return (rc == Z_STREAM_END);
}

#ifdef LIBBZ
bool decompress_libbz
	(const TMCG_OpenPGP_Message* msg, tmcg_openpgp_octets_t &infmsg)
{
	int rc = 0;
	bz_stream zs;
	char zin[4096];
	char zout[4096];
	zs.bzalloc = NULL;
	zs.bzfree = NULL;
	zs.opaque = NULL;
	zs.avail_in = 0;
	zs.next_in = NULL;
	rc = BZ2_bzDecompressInit(&zs, 0, 0);
	if (rc != BZ_OK)
	{
		if (opt_verbose)
			std::cerr << "BZLIB ERROR: " << (int)rc << std::endl;
		return false;
	}
	size_t cnt = 0;
	memset(zin, 0, sizeof(zin));
	do
	{
		if (zs.avail_in == 0)
		{
			size_t zlen = 0;
			for (size_t i = 0; i < sizeof(zin); i++)
			{
				if (cnt >= (msg->compressed_data).size())
					break;
				zin[i] = (msg->compressed_data)[cnt];
				zlen++, cnt++;
			}
			zs.avail_in = zlen;
			zs.next_in = zin;
		}
		memset(zout, 0, sizeof(zout));
		zs.avail_out = sizeof(zout);
		zs.next_out = zout;
		rc = BZ2_bzDecompress(&zs);
		if ((rc == BZ_DATA_ERROR) || (rc == BZ_DATA_ERROR_MAGIC) ||
			(rc == BZ_MEM_ERROR))
		{
			if (opt_verbose)
				std::cerr << "BZLIB ERROR: " << rc << std::endl;
			BZ2_bzDecompressEnd(&zs);
			return false;
		}
		for (size_t i = 0; i < (sizeof(zout) - zs.avail_out); i++)
			infmsg.push_back(zout[i]);
	}
	while ((rc != BZ_STREAM_END) && (rc != BZ_PARAM_ERROR));
	BZ2_bzDecompressEnd(&zs);
	return (rc == BZ_STREAM_END);
}
#endif


bool decrypt
	(const std::vector<std::string> &args,
	 const tmcg_openpgp_secure_string_t &passphrase,
	 const tmcg_openpgp_octets_t &ciphertext)
{
	// autodetect ASCII armor
	std::string input_str, armored_message;
	for (size_t i = 0; ((i < ciphertext.size()) && (i < 20)); i++)
		input_str += ciphertext[i];
	if (input_str.find("-----BEGIN PGP") == 0)
	{
		for (size_t i = 0; i < ciphertext.size(); i++)
			armored_message += ciphertext[i];
		armored_message += "\r\n";	
	}
	else
	{
		CallasDonnerhackeFinneyShawThayerRFC4880::
			ArmorEncode(TMCG_OPENPGP_ARMOR_MESSAGE, ciphertext,
				armored_message);
	}
	// parse OpenPGP message
	TMCG_OpenPGP_Message *msg = NULL;
	if (!CallasDonnerhackeFinneyShawThayerRFC4880::
		MessageParse(armored_message, opt_verbose, msg))
	{
		std::cerr << "ERROR: message parsing failed (1)" << std::endl;
		return false;
	}
	if (opt_verbose > 1)
		msg->PrintInfo();
	if (msg->encrypted_message.size() == 0)
	{
		std::cerr << "ERROR: no encrypted data found" << std::endl;
		delete msg;
		return false;
	}
	// decrypt session key
	tmcg_openpgp_secure_octets_t seskey;
	bool seskey_decrypted = false;
	if (opt_with_session_key.size() > 0)
		seskey_decrypted = true;
	for (size_t i = 0; (i < args.size()) && !seskey_decrypted; i++)
	{
		std::string armored_key;
		if (!autodetect_file(args[i], TMCG_OPENPGP_ARMOR_PRIVATE_KEY_BLOCK,
			armored_key))
		{
			delete msg;
			return false;
		}
		TMCG_OpenPGP_Prvkey *prv = NULL;
		if (!CallasDonnerhackeFinneyShawThayerRFC4880::
			PrivateKeyBlockParse(armored_key, opt_verbose, passphrase, prv))
		{
			delete msg;
			return false;
		}
		for (size_t k = 0; k < prv->private_subkeys.size(); k++)
		{
			TMCG_OpenPGP_PrivateSubkey *ssb = prv->private_subkeys[k];
			// try to decrypt the session key for all PKESK packets
			for (size_t j = 0; j < (msg->PKESKs).size(); j++)
			{
				if (ssb->Decrypt(msg->PKESKs[j], opt_verbose, seskey))
				{
					seskey_decrypted = true;
					msg->decryptionfpr.clear();
					msg->decryptionfpr.insert(msg->decryptionfpr.end(),
						prv->pub->fingerprint.begin(),
						prv->pub->fingerprint.end());
					break;
				}
			}
			if (seskey_decrypted)
				break;
		}
		delete prv;
	}
	if (!seskey_decrypted && !decrypt_session_key(msg, seskey))
	{
		std::cerr << "ERROR: session key decryption failed" << std::endl;
		delete msg;
		return false;
	}
	// use session keys provided by option --with-session-key
	for (size_t k = 0; k < opt_with_session_key.size(); k++)
	{
		size_t col = opt_with_session_key[k].find(":");
		size_t kst = 0;
		tmcg_openpgp_skalgo_t algo = TMCG_OPENPGP_SKALGO_PLAINTEXT;
		seskey.clear();
		if ((col != opt_with_session_key[k].npos) && (col > 0))
		{
			std::string tmp = opt_with_session_key[k].substr(0, col);
			algo = (tmcg_openpgp_skalgo_t)strtoul(tmp.c_str(), NULL, 10);
			seskey.push_back(algo);
			kst = col + 1;
		}
		else if ((col != opt_with_session_key[k].npos) && (col == 0))
		{
			kst = 1;
		}
		if (opt_verbose)
		{
			std::cerr << "INFO: use algorithm " << (int)algo << " and" <<
				" session key provided by user" << std::endl;
		}
		bool first = true;
		tmcg_openpgp_secure_string_t hex;
		for (size_t i = kst; i < opt_with_session_key[k].length(); i++)
		{
			if (first)
			{
				hex = opt_with_session_key[k][i];
				first = false;
			}
			else
			{
				hex += opt_with_session_key[k][i];
				hex = strtoul(hex.c_str(), NULL, 16);
				seskey.push_back(hex[0]);
				first = true;
			}
		}
		tmcg_openpgp_octets_t data;
		if (msg->Decrypt(seskey, 0, data))
			break;
	}
	// decrypt OpenPGP message
	tmcg_openpgp_octets_t data;
	if (!msg->Decrypt(seskey, opt_verbose, data))
	{
		std::cerr << "ERROR: message decryption failed" << std::endl;
		delete msg;
		return false;
	}
	if (!CallasDonnerhackeFinneyShawThayerRFC4880::
		MessageParse(data, opt_verbose, msg))
	{
		std::cerr << "ERROR: message parsing failed (2)" << std::endl;
		delete msg;
		return false;
	}
	if (opt_verbose > 1)
		msg->PrintInfo();
	// decompress OpenPGP message
	if ((msg->compressed_data).size() != 0)
	{
		tmcg_openpgp_octets_t data;
		bool decompress_ok = false;
		switch (msg->compalgo)
		{
			case TMCG_OPENPGP_COMPALGO_UNCOMPRESSED:
				for (size_t i = 0; i < (msg->compressed_data).size(); i++)
					data.push_back(msg->compressed_data[i]);
				decompress_ok = true; // no compression
				break;
			case TMCG_OPENPGP_COMPALGO_ZIP:
			case TMCG_OPENPGP_COMPALGO_ZLIB:
				decompress_ok = decompress_libz(msg, data);
				break;
#ifdef LIBBZ
			case TMCG_OPENPGP_COMPALGO_BZIP2:
				decompress_ok = decompress_libbz(msg, data);
				break;
#endif
			default:
				if (opt_verbose > 1)
				{
					std::cerr << "WARNING: compression algorithm " <<
						(int)msg->compalgo << " is not supported" <<
						std::endl;
				}
				break;
		}
		if (!decompress_ok)
		{
			std::cerr << "ERROR: decompress failed" << std::endl;
			delete msg;
			return false;
		}
		if (!CallasDonnerhackeFinneyShawThayerRFC4880::MessageParse(data,
			opt_verbose, msg))
		{
			std::cerr << "ERROR: message parsing failed (3)" << std::endl;
			delete msg;
			return false;
		}
		if (opt_verbose > 1)
			msg->PrintInfo();
	}
	// handle decompressed message
	if ((msg->literal_data).size() == 0)
	{
		std::cerr << "ERROR: no literal data found" << std::endl;
		delete msg;
		return false;
	}
// TODO: verify included signatures	
	// copy the content of literal data packet
	tmcg_openpgp_octets_t content;
	content.insert(content.end(),
		(msg->literal_data).begin(), (msg->literal_data).end());
	if ((msg->filename == "_CONSOLE") && opt_verbose)
	{
		std::cerr << "INFO: sender requested \"for-your-eyes-only\"" <<
			std::endl;
	}
	// output the result
	for (size_t i = 0; i < content.size(); i++)
			std::cout << content[i];
	return true;
}

int main
	(int argc, char **argv)
{
	static const char *usage = "dkg-sop [OPTIONS] SUBCOMMAND [ARGS]";
	static const char *about = PACKAGE_STRING " " PACKAGE_URL;
	static const char *version = "dkg-sop " PACKAGE_VERSION;
	std::string subcmd;
	std::vector<std::string> args;
	bool opt_passphrase = false;
	bool end_of_options = false;
	opt_not_before = 0;
	opt_not_after = time(NULL);

	for (size_t i = 0; i < (size_t)(argc - 1); i++)
	{
		std::string arg = argv[i+1];
		if (!end_of_options && ((arg.find("--") == 0) || (arg.find("-") == 0)))
		{
			if (arg == "--")
			{
				end_of_options = true;
				continue;
			}
			// read options
			if ((arg.find("-h") == 0) || (arg.find("--help") == 0))
			{
				std::cout << usage << std::endl;
				std::cout << about << std::endl;
				std::cout << "Arguments mandatory for long options are also" <<
					" mandatory for short options." << std::endl;
				std::cout << "  -h, --help           print this help" <<
					std::endl;
				std::cout << "  -n, --no-rfc4880bis  disable features of RFC" <<
					" 4880bis" << std::endl;
				std::cout << "  -P, --passphrase     ask user for passphrase" <<
					" protecting private key material" << std::endl;
				std::cout << "  -V, --verbose        increase verbosity" <<
					std::endl;
				return 0; // not continue
			}
			if ((arg.find("-n") == 0) || (arg.find("--no-rfc4880bis") == 0))
			{
				opt_rfc4880bis = false; // disable some features of RFC 4880bis
				continue;
			}
			if ((arg.find("-P") == 0) || (arg.find("--passphrase") == 0))
			{
				opt_passphrase = true; // ask user for passphrase
				continue;
			}
			if ((arg.find("-V") == 0) || (arg.find("--verbose") == 0))
			{
				opt_verbose++; // increase verbosity
				continue;
			}
			// The following options are defined in [DKG20].
			if (arg.find("--no-armor") == 0)
			{
				opt_armor = false; // disable ASCII-armored output
				continue;
			}
			if (arg.find("--as=binary") == 0)
			{
				opt_as_binary = true; // choose format "binary" (default)
				opt_as_text = false;
				continue;
			}
			if (arg.find("--as=text") == 0)
			{
				opt_as_binary = false;
				opt_as_text = true; // choose format "text"
				continue;
			}
			if (arg.find("--as=mime") == 0)
			{
				opt_as_binary = false;
				opt_as_mime = true; // choose format "mime"
				continue;
			}
			if (arg.find("--not-before=") == 0)
			{
				std::string s = arg.substr(13);
				if (!timestamp(s, opt_not_before))
				{
					std::cerr << "ERROR: wrong timestamp" <<
						" format" << std::endl;
					return -1;
				}
				continue;
			}
			if (arg.find("--not-after=") == 0)
			{
				std::string s = arg.substr(12);
				if (!timestamp(s, opt_not_after))
				{
					std::cerr << "ERROR: wrong timestamp" <<
						" format" << std::endl;
					return -1;
				}
				continue;
			}
			if (arg.find("--with-password=") == 0)
			{
				std::string s = arg.substr(16);
				if (!valid_utf8(s))
				{
					std::cerr << "ERROR: invalid UTF-8" <<
						" encoding found in password" <<
						std::endl;
					return 31;
				}
				opt_with_password.push_back(s);
				continue;
			}
			if (arg.find("--sign-with=") == 0)
			{
				std::string s = arg.substr(12);
				opt_sign_with.push_back(s);
				continue;
			}
			if (arg.find("--with-session-key=") == 0)
			{
				std::string s = arg.substr(19);
				opt_with_session_key.push_back(s);
				continue;	
			}
			if (arg.find("--verify-not-before=") == 0)
			{
				std::string s = arg.substr(20);
				if (!timestamp(s, opt_not_before))
				{
					std::cerr << "ERROR: wrong timestamp" <<
						" format" << std::endl;
					return -1;
				}
				continue;
			}
			if (arg.find("--verify-not-after=") == 0)
			{
				std::string s = arg.substr(19);
				if (!timestamp(s, opt_not_after))
				{
					std::cerr << "ERROR: wrong timestamp" <<
						" format" << std::endl;
					return -1;
				}
				continue;
			}
			if (arg.find("--verify-out=") == 0)
			{
				opt_verify_out = arg.substr(14);
				// If the designated file already exists in the filesystem,
				// "sop decrypt" will fail with "OUTPUT_EXISTS". [DKG20]
				std::ifstream ifs(opt_verify_out.c_str(), std::ifstream::in);
				if (ifs.is_open())
				{
					ifs.close();
					std::cerr << "ERROR: output file already exists" <<
						std::endl;
					return 59;
				}
				continue;		
			}
			if (arg.find("--verify-with=") == 0)
			{
				std::string s = arg.substr(14);
				opt_verify_with.push_back(s);
				continue;
			}
			// If a "sop" implementation does not handle a supplied option for
			// a given subcommand, it fails with "UNSUPPORTED_OPTION". [DKG20]
			std::cerr << "ERROR: SOP option \"" << arg << "\" not" <<
				" supported" << std::endl;
			return 37;
		}
		else
		{
			// read arguments
			if (subcmd.length() == 0)
			{
				subcmd = arg; // 1st argument is the SOP subcommand
			}
			else
			{
				if (!valid_utf8(arg))
				{
					std::cerr << "ERROR: invalid UTF-8 encoding found at" <<
						" argument #" << (i+1) << std::endl;
					return 53;
				}
				args.push_back(arg);
			}
		}
	}

	// lock memory
	bool force_secmem = false, should_unlock = false;
	if (!lock_memory())
	{
		std::cerr << "WARNING: locking memory failed; CAP_IPC_LOCK required" <<
			" for full memory protection" << std::endl;
		// at least try to use libgcrypt's secure memory
		force_secmem = true;
	}
	else
		should_unlock = true;

	// initialize LibTMCG
	if (!init_libTMCG(force_secmem))
	{
		std::cerr << "ERROR: initialization of LibTMCG failed" << std::endl;
		if (should_unlock)
			unlock_memory();
		return -1;
	}
	if (opt_verbose)
	{
		std::cerr << "INFO: using LibTMCG version " << version_libTMCG() <<
			std::endl;
	}

	// read passphrase
	tmcg_openpgp_secure_string_t passphrase;
	if (opt_passphrase)
	{
		tmcg_openpgp_secure_string_t passphrase_check;
		std::string ps1 = "Please enter the passphrase for the private key";
		std::string ps2 = "Please repeat the passphrase to continue";
		do
		{
			passphrase = "", passphrase_check = "";
			if (!get_passphrase(ps1, false, passphrase))
			{
				if (should_unlock)
					unlock_memory();
				return -1;
			}
			if (!get_passphrase(ps2, false, passphrase_check))
			{
				if (should_unlock)
					unlock_memory();
				return -1;
			}
			if (passphrase != passphrase_check)
			{
				std::cerr << "WARNING: passphrase does not match;" <<
					" please try again" << std::endl;
			}
			else if (passphrase == "")
			{
				std::cerr << "WARNING: private key protection disabled" <<
					" due to given empty passphrase" << std::endl;
			}
		}
		while (passphrase != passphrase_check);
	}

	// perpare input, execute corresponding subcommand, and evaluate return code
	int ret = 0;
	if (subcmd == "version")
	{
		// Version Information [DKG20]
		// The version string emitted should contain the name of the "sop"
		// implementation, followed by a single space, followed by the version
		// number. A "sop" implementation should use a version number that
		// respects an established standard that is easily comparable and
		// parsable, like [SEMVER].
		std::cout << version << std::endl;
	}
	else if (subcmd == "generate-key")
	{
		// Generate a Secret Key [DKG20]
		//   Standard Input: ignored
		//   Standard Output: "KEY"
		// Generate a single default OpenPGP key with zero or more User IDs.
		// The generared secret key SHOULD be usable for as much of the "sop"
		// functionality as possible.
		if (!generate(args, passphrase))
			ret = -1;
	}
	else if (subcmd == "extract-cert")
	{
		// Extract a Certificate from a Secret Key [DKG20]
		//   Standard Input: "KEY"
		//   Standard Output: "CERTS"
		// Note that the resultant "CERTS" object will only ever contain one
   		// OpenPGP certificate, since "KEY" contains exactly one OpenPGP
		// Transferable Secret Key.
		if (!extract(passphrase))
			ret = -1;
	}
	else if (subcmd == "sign")
	{
		// Create Detached Signatures [DKG20]
		//   Standard Input: "DATA"
		//   Standard Output: "SIGNATURES"
		// Exactly one signature will be made by each supplied "KEY".
		// "--as" defaults to "binary". If "--as=text" and the input "DATA" is
		// not valid "UTF-8", "sop sign" fails with "EXPECTED_TEXT".
		tmcg_openpgp_octets_t data;
		char c;
		while (std::cin.get(c))
			data.push_back(c);
		if (opt_as_text)
		{
			if (!valid_utf8(data))
			{
				std::cerr << "ERROR: invalid UTF-8 encoding found" << std::endl;
				ret = 53;
			}
		}
		if ((ret == 0) && !sign(args, passphrase, data))
			ret = -1;
	}
	else if (subcmd == "verify")
	{
		// Verify Detached Signatures [DKG20]
		//  Standard Input: "DATA"
		//  Standard Output: "VERIFICATIONS"
		// "--not-before" and "--not-after" indicate that signatures
		// with dates outside certain range MUST NOT be considered
		// valid.
		// "--not-before" defaults to the beginning of time. Accepts
		// the special value "-" to indicate the beginning of time
		// (i.e. no lower boundary).
		// "--not-after" defaults to the current system time ("now").
		// Accepts the special value "-" to indicate the end of time
		// (i.e. no upper boundary).
		// "sop verify" only returns "OK" if at least one certificate
		// included in any "CERTS" object made a valid signature in
		// the range over the "DATA" supplied.
		// For details about the valid signatures, the user MUST
		// inspect the "VERIFICATIONS" output.
		tmcg_openpgp_octets_t data;
		std::vector<std::string> verifications;
		char c;
		while (std::cin.get(c))
			data.push_back(c);
		// If no "CERTS" are supplied, "sop verify" fails with
		// "MISSING_ARG". [DKG20]
		if (args.size() < 2)
		{
			std::cerr << "ERROR: Missing required argument" << std::endl;
			ret = 19;
		}
		if ((ret == 0) && !verify(args, data, verifications))
			ret = -1;
		// If no valid signatures are found, "sop verify" fails with
		// "NO_SIGNATURE". [DKG20]
		if ((ret == 0) && (verifications.size() < 1))
		{
			std::cerr << "ERROR: No acceptable signatures found" << std::endl;
			ret = 3;
		}
		for (size_t i = 0; i < verifications.size(); i++)
			std::cout << verifications[i] << std::endl;
	}
	else if (subcmd == "encrypt")
	{
		// Encrypt a Message [DKG20]
		//   Standard Input: "DATA"
		//   Standard Output: "CIPHERTEXT"
		// "--as" defaults to "binary". The setting of "--as"
		// corresponds to the one octet format field found in
		// the Literal Data packet at the core of the output
		// "CIPHERTEXT". If "--as" is set to "binary", the
		// octet is "b" ("0x62"). If it is "text", the format
		// octet is "u" ("0x75").  If it is "mime", the format
		// octet is "m" ("0x6d").
		// "--with-password" enables symmetric encryption (and
		// can be used multiple times if multiple passwords are
		// desired).  If "sop encrypt" encounters a "PASSWORD"
		// which is not a valid "UTF-8" string, or is otherwise
		// not robust in its representation to humans, it fails
		// with "PASSWORD_NOT_HUMAN_READABLE". If "sop encrypt"
		// sees trailing whitespace at the end of a "PASSWORD",
		// it will trim the trailing whitespace before using the
		// password.
		// "--sign-with" creates exactly one signature by the
		// identified secret key (and can be used multiple times
		// if signatures from multiple keys are desired).
		// If "--as" is set to "binary", then "--sign-with" will
		// sign as a binary document (OpenPGP signature type "0x00").
		// If "--as" is set to "text", then "--sign-with" will
		// sign as a canonical text document (OpenPGP signature
		// type "0x01"). In this case, if the input "DATA" is not
		// valid "UTF-8", "sop encrypt" fails with "EXPECTED_TEXT".
		tmcg_openpgp_octets_t data;
		char c;
		while (std::cin.get(c))
			data.push_back(c);
		if (opt_as_text)
		{
			if (!valid_utf8(data))
			{
				std::cerr << "ERROR: invalid UTF-8 encoding found" << std::endl;
				ret = 53;
			}
		}
		if ((ret == 0) && !encrypt(args, passphrase, data))
			ret = encrypt_ret;
	}
	else if (subcmd == "decrypt")
	{
		// Decrypt a Message [DKG20]
		//   Standard Input: "CIPHERTEXT"
		//   Standard Output: "DATA"
		// "--with-password" enables decryption based on any "SKESK" packets
		// in the "CIPHERTEXT". This option can be used multiple times if the
		// user wants to try more than one password.
		// "--verify-out" produces signature verification status to the
		// designated file. If the designated file already exists in the
		// filesystem, "sop decrypt" will fail with "OUTPUT_EXISTS".
		// The return code of "sop decrypt" is not affected by the results of
		// signature verification. The caller MUST check the returned
		// "VERIFICATIONS" to confirm signature status. An empty
		// "VERIFICATIONS" output indicates that no valid signatures were found.
		// "--verify-with" identifies a set of certificates whose signatures
		// would be acceptable for signatures over this message.
		// If the caller is interested in signature verification, both
		// "--verify-out" and at least one "--verify-with" must be supplied. If
		// only one of these arguments is supplied, "sop decrypt" fails with
		// "INCOMPLETE_VERIFICATION".
		// "--verify-not-before" and "--verify-not-after" provide a date range
		// for acceptable signatures, by analogy with the options for "sop
		// verify". They should only be supplied when doing signature
		// verification.
		tmcg_openpgp_octets_t ciphertext;
		char c;
		while (std::cin.get(c))
			ciphertext.push_back(c);
		// If no "KEY" or "--with-password" or "--with-session-key" options are
		// present, "sop decrypt" fails with "MISSING_ARG". [DKG20]
		if ((args.size() < 1) && (opt_with_password.size() < 1) &&
			(opt_with_session_key.size() < 1))
		{
			std::cerr << "ERROR: Missing required argument" << std::endl;
			ret = 19;
		}
		// If unable to decrypt, "sop decrypt" fails with "CANNOT_DECRYPT".
		if ((ret == 0) && !decrypt(args, passphrase, ciphertext))
			ret = 29; // "CANNOT_DECRYPT" [DKG20]
	}
	else
	{
		// If the user supplies a subcommand that "sop" does not implement, it
		// fails with "UNSUPPORTED_SUBCOMMAND". [DKG20]
		std::cerr << "ERROR: SOP subcommand \"" << subcmd << "\" not" <<
			" supported" << std::endl;
		ret = 69;
	}

	// finish
	if (should_unlock)
		unlock_memory();	
	return ret;
}

