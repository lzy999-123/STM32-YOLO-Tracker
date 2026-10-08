from pathlib import Path
crypto_header = Path('/work/kernel/include/linux/crypto.h')
crypto_header.write_text(crypto_header.read_text().replace('#define CRYPTO_MINALIGN ARCH_KMALLOC_MINALIGN', '#define CRYPTO_MINALIGN 8'))
p = Path('/work/kernel/net/wireless/lib80211_crypt_ccmp.c')
s = p.read_text()
old = '''static int __init lib80211_crypto_ccmp_init(void)
{
	return lib80211_register_crypto_ops(&lib80211_crypt_ccmp);
}'''
new = '''static int __init lib80211_crypto_ccmp_init(void)
{
	struct crypto_aead *tfm;
	/* Instantiate the cipher here, where module loading may sleep.
	 * The driver first uses CCMP from the atomic transmit path. */
	tfm = crypto_alloc_aead("ccm(aes)", 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm)) {
		pr_err("lib80211 CCMP cipher unavailable: %ld\\n", PTR_ERR(tfm));
		return PTR_ERR(tfm);
	}
	crypto_free_aead(tfm);
	return lib80211_register_crypto_ops(&lib80211_crypt_ccmp);
}'''
if old in s:
    p.write_text(s.replace(old, new))
elif 'CCMP cipher unavailable' not in s:
    raise SystemExit('CCMP initializer not found')
print('CCMP preflight prepared')
