 /* Copyright (C) 2004-2006, Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/crypto.h>
#include <linux/cryptohash.h>
#include <linux/spinlock.h>
#include <linux/scatterlist.h>
#include <crypto/scatterwalk.h>
#include <crypto/algapi.h>
#include <crypto/aes.h>
#include <crypto/des.h>
#include <crypto/sha.h>
#include <crypto/hash.h>
#include <crypto/internal/hash.h>

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/clk.h>

#include <mach/map.h>
#include <mach/regs-clock.h>
#include <mach/regs-crypto.h>

/* Static structures */

#define DMA_BUFSZ			(4096)

struct nuc970_crypto_dev {
	
	struct nuc970_crypto_regs  *regs;
	spinlock_t 	 aes_lock;
	spinlock_t 	 des_lock;
	spinlock_t 	 sha_lock;
	
	u8			 aes_channels;
	u8           des_channels;
	
    u32          *aes_inbuf;
    dma_addr_t   aes_inbuf_dma_addr;
    u32          *aes_outbuf;
    dma_addr_t   aes_outbuf_dma_addr;
	
    u32          *des_inbuf;
    dma_addr_t   des_inbuf_dma_addr;
    u32          *des_outbuf;
    dma_addr_t   des_outbuf_dma_addr;

    u32          *hmac_inbuf;
    dma_addr_t   hmac_inbuf_dma_addr;
}  nuc970_crdev;


struct nuc970_ctx {
	int		channel;
	u32		mode;
	u32     keysize;
	struct nuc970_aes_regs  *aes_regs;
	struct nuc970_tdes_regs  *tdes_regs;
	u32		aes_ctrl;
	u32		tdes_ctrl;
	int		sha_remain_bytes;
	int     is_first_block;
};

struct cryp_algo_template {
	u32   algomode;
	struct crypto_alg crypto;
};


void dump_regs(void)
{
	struct nuc970_crypto_regs  *crpt_regs = nuc970_crdev.regs;
	
	printk("crpt_regs = 0x%x\n", (u32)crpt_regs);
	printk("CRPT_INTSTS = 0x%x\n", crpt_regs->CRPT_INTSTS);
	printk("CRPT_AES_CTL = 0x%x\n", crpt_regs->CRPT_AES_CTL);
	printk("CRPT_AES_STS = 0x%x\n", crpt_regs->CRPT_AES_STS);
	printk("CRPT_AES0_KEY0 = 0x%x\n", crpt_regs->CRPT_AES0_KEY0);
	printk("CRPT_AES0_KEY1 = 0x%x\n", crpt_regs->CRPT_AES0_KEY1);
	printk("CRPT_AES0_KEY2 = 0x%x\n", crpt_regs->CRPT_AES0_KEY2);
	printk("CRPT_AES0_KEY3 = 0x%x\n", crpt_regs->CRPT_AES0_KEY3);
	printk("CRPT_AES0_SADDR = 0x%x\n", crpt_regs->CRPT_AES0_SADDR);
	printk("CRPT_AES0_DADDR = 0x%x\n", crpt_regs->CRPT_AES0_DADDR);
	printk("CRPT_AES0_CNT = 0x%x\n", crpt_regs->CRPT_AES0_CNT);
}


static int nuc970_do_aes_crypt(struct ablkcipher_request *areq, u32 encrypt)
{
	struct nuc970_ctx *ctx = crypto_ablkcipher_ctx(crypto_ablkcipher_reqtfm(areq));
	struct nuc970_crypto_regs  *crpt_regs = nuc970_crdev.regs;
	struct nuc970_aes_regs *aes_regs = ctx->aes_regs;
	struct scatterlist   *in_sg, *out_sg;
	int  i, req_len, dma_len, count;
	int  timeout = 100000;

	//printk("[%s],ctx=0x%x, chn=%d\n", __func__, (int)ctx, ctx->channel);
	
	BUG_ON(!areq->info);

	spin_lock(&nuc970_crdev.aes_lock);
	
	crpt_regs->CRPT_AES_CTL = 0;
	crpt_regs->CRPT_INTSTS = (AESIF | AESERRIF);

	for (i = 0; i < 4; i++)
	{
		aes_regs->iv[i] = *(u32 *)((u32)areq->info + i * 4);
		//printk("AES IV %d = %08x\n", i, aes_regs->iv[i]);
	}
	
	crpt_regs->CRPT_AES_CTL = ctx->keysize | ctx->mode | AES_INSWAP | AES_OUTSWAP |
	                          AES_DMAEN | (ctx->channel << 24);

	if (ctx->is_first_block)
		ctx->is_first_block = 0;
	else
		crpt_regs->CRPT_AES_CTL |= AES_DMACSCAD;
									
	if (encrypt)
		crpt_regs->CRPT_AES_CTL |= AES_ENCRYPT;
	
	in_sg = areq->src;
	out_sg = areq->dst;
	req_len = areq->nbytes;
	
	while ((req_len > 0) && in_sg)
	{
		dma_len = min(in_sg->length, req_len);
		
		aes_regs->count = dma_len;
		aes_regs->src_addr = nuc970_crdev.aes_inbuf_dma_addr;
		aes_regs->dst_addr = nuc970_crdev.aes_outbuf_dma_addr;

		count = sg_copy_to_buffer(in_sg, 1, nuc970_crdev.aes_inbuf, dma_len);
		if (count != dma_len)
		{
			printk("sg in buffer error!\n");
			break;
		}	
		
		in_sg = sg_next(in_sg);
		
		crpt_regs->CRPT_AES_CTL |= AES_START;
		
		while ((crpt_regs->CRPT_AES_CTL & AES_START) && timeout--)
		{
			cpu_relax();
		}
		
		if (timeout == 0)
		{
			printk("Crypto AES engine failed!\n");
			spin_unlock(&nuc970_crdev.aes_lock);
			return 1;
		}

		count = sg_copy_from_buffer(out_sg, 1, nuc970_crdev.aes_outbuf, dma_len);
		if (count != dma_len)
		{
			printk("sg out buffer error!\n");
			break;
		}	
		req_len -= 	count;			
	}

	spin_unlock(&nuc970_crdev.aes_lock);
	
	return 0;
}

static int nuc970_aes_decrypt(struct ablkcipher_request *areq)
{
	return nuc970_do_aes_crypt(areq, 0);
}

static int nuc970_aes_encrypt(struct ablkcipher_request *areq)
{
	return nuc970_do_aes_crypt(areq, 1);
}

static int nuc970_aes_setkey(struct crypto_ablkcipher *cipher,
				 const u8 *key, unsigned int keylen)
{
	struct nuc970_ctx  *ctx = crypto_ablkcipher_ctx(cipher);
	u32 *flags = &cipher->base.crt_flags;
	struct nuc970_aes_regs *aes_regs = ctx->aes_regs;
	int  i;

	//printk("[%s],ctx=0x%x, chn=%d\n", __func__, (int)ctx, ctx->channel);

	switch (keylen) 
	{
		case AES_KEYSIZE_128:
			ctx->keysize = AES_KEYSZ_128;
			break;

		case AES_KEYSIZE_192:
			ctx->keysize = AES_KEYSZ_192;
			break;

		case AES_KEYSIZE_256:
			ctx->keysize = AES_KEYSZ_256;
			break;

		default:
			printk("[%s]: Unsupported keylen %d!\n", __func__, keylen);
			*flags |= CRYPTO_TFM_RES_BAD_KEY_LEN;
			return -EINVAL;
	}

	//printk("aes_regs = 0x%x\n", (u32)aes_regs);	
	for (i = 0; i < keylen/4; i++)
	{
		aes_regs->key[i] = *(u32 *)(key + i * 4);
		//printk("AES KEY %d = 0x%x, 0x%x\n", i, aes_regs->key[i], *(u32 *)(key + i * 4));
	}
	return 0;
}

static int nuc970_aes_init(struct crypto_tfm *tfm)
{
	struct nuc970_ctx  *ctx = crypto_tfm_ctx(tfm);
	struct crypto_alg *alg = tfm->__crt_alg;
	struct cryp_algo_template *cryp_alg = container_of(alg, struct cryp_algo_template, crypto);
	int   chn;
	
	spin_lock(&nuc970_crdev.aes_lock);

	for (chn = 0; chn < 4; chn++)
	{
		if ((nuc970_crdev.aes_channels & (1 << chn)) == 0)
		{
			nuc970_crdev.aes_channels |= (1 << chn);
			break;
		}
	}	
	if (chn >= 4)
	{
		spin_unlock(&nuc970_crdev.aes_lock);
		return -ENOMEM;
	}
	
	ctx->mode = cryp_alg->algomode;
	ctx->channel = chn;
	ctx->aes_ctrl = 0;
	ctx->aes_regs = (struct nuc970_aes_regs *)((u32)nuc970_crdev.regs + 0x110 + (0x3c * chn));
	ctx->is_first_block = 1;

	spin_unlock(&nuc970_crdev.aes_lock);

	//printk("[%s],ctx=0x%x, chn=%d\n", __func__, (int)ctx, ctx->channel);

	return 0;
}

static void nuc970_aes_exit(struct crypto_tfm *tfm)
{
	struct nuc970_ctx  *ctx = crypto_tfm_ctx(tfm);

	spin_lock(&nuc970_crdev.aes_lock);
	nuc970_crdev.aes_channels &= ~(1 << ctx->channel);
	nuc970_crdev.regs->CRPT_AES_CTL = AES_STOP;
	spin_unlock(&nuc970_crdev.aes_lock);
}

static int nuc970_do_des_crypt(struct ablkcipher_request *areq, u32 encrypt)
{
	struct nuc970_ctx *ctx = crypto_ablkcipher_ctx(crypto_ablkcipher_reqtfm(areq));
	struct nuc970_crypto_regs  *crpt_regs = nuc970_crdev.regs;
	struct nuc970_tdes_regs *tdes_regs = ctx->tdes_regs;
	struct scatterlist   *in_sg, *out_sg;
	int  i, req_len, dma_len, count;
	int  timeout = 100000;

	//printk("[%s],ctx=0x%x, chn=%d\n", __func__, (int)ctx, ctx->channel);
	
	BUG_ON(!areq->info);

	spin_lock(&nuc970_crdev.des_lock);
	
	crpt_regs->CRPT_TDES_CTL = 0;
	crpt_regs->CRPT_INTSTS = (TDESIF | TDESERRIF);

	for (i = 0; i < 2; i++)
	{
		tdes_regs->iv[i] = *(u32 *)((u32)areq->info + i * 4);
		//printk("DES/TDES IV %d = %08x\n", i, tdes_regs->iv[i]);
	}
	
	crpt_regs->CRPT_TDES_CTL = ctx->mode | TDES_INSWAP | TDES_OUTSWAP | TDES_BLKSWAP |
	                            TDES_DMAEN | (ctx->channel << 24);

	if (ctx->is_first_block)
		ctx->is_first_block = 0;
	else
		crpt_regs->CRPT_TDES_CTL |= TDES_DMACSCAD;
									
	if (encrypt)
		crpt_regs->CRPT_TDES_CTL |= TDES_ENCRYPT;
	
	in_sg = areq->src;
	out_sg = areq->dst;
	req_len = areq->nbytes;
	
	while ((req_len > 0) && in_sg)
	{
		dma_len = min(in_sg->length, req_len);
		
		tdes_regs->count = dma_len;
		tdes_regs->src_addr = nuc970_crdev.des_inbuf_dma_addr;
		tdes_regs->dst_addr = nuc970_crdev.des_outbuf_dma_addr;

		count = sg_copy_to_buffer(in_sg, 1, nuc970_crdev.des_inbuf, dma_len);
		if (count != dma_len)
		{
			printk("sg in buffer error!\n");
			break;
		}	
		
		in_sg = sg_next(in_sg);
		
		crpt_regs->CRPT_TDES_CTL |= TDES_START;
		
		while ((crpt_regs->CRPT_TDES_CTL & TDES_START) && timeout--)
		{
			cpu_relax();
		}
		
		if (timeout == 0)
		{
			printk("Crypto DES/TDES engine failed!\n");
			spin_unlock(&nuc970_crdev.des_lock);
			return 1;
		}

		count = sg_copy_from_buffer(out_sg, 1, nuc970_crdev.des_outbuf, dma_len);
		if (count != dma_len)
		{
			printk("sg out buffer error!\n");
			break;
		}	
		req_len -= 	count;			
	}

	spin_unlock(&nuc970_crdev.des_lock);
	
	return 0;
}

static int nuc970_des_decrypt(struct ablkcipher_request *areq)
{
	return nuc970_do_des_crypt(areq, 0);
}

static int nuc970_des_encrypt(struct ablkcipher_request *areq)
{
	return nuc970_do_des_crypt(areq, 1);
}

static int nuc970_des_setkey(struct crypto_ablkcipher *cipher,
				 const u8 *key, unsigned int keylen)
{
	struct nuc970_ctx  *ctx = crypto_ablkcipher_ctx(cipher);
	u32 *flags = &cipher->base.crt_flags;
	struct nuc970_tdes_regs *tdes_regs = ctx->tdes_regs;
	int  i;

	//printk("[%s],ctx=0x%x, chn=%d\n", __func__, (int)ctx, ctx->channel);
	
	if (keylen == 3 * DES_KEY_SIZE)
		ctx->mode |= TDES_TMODE;         /* is Tripple DES */
	else if (keylen != DES_KEY_SIZE)
	{
		printk("[%s]: Unsupported keylen %d!", __func__, keylen);
		*flags |= CRYPTO_TFM_RES_BAD_KEY_LEN;
		return -EINVAL;
    }

	//printk("tdes_regs = 0x%x\n", (u32)tdes_regs);	
	for (i = 0; i < keylen/4; i++)
	{
		tdes_regs->key[i] = *(u32 *)(key + i * 4);
		//printk("DES/TDES KEY %d = 0x%x, 0x%x\n", i, tdes_regs->key[i], *(u32 *)(key + i * 4));
	}
	return 0;
}

static int nuc970_des_init(struct crypto_tfm *tfm)
{
	struct nuc970_ctx  *ctx = crypto_tfm_ctx(tfm);
	struct crypto_alg *alg = tfm->__crt_alg;
	struct cryp_algo_template *cryp_alg = container_of(alg, struct cryp_algo_template, crypto);
	int   chn;
	
	//printk("[%s]\n", __func__);

	spin_lock(&nuc970_crdev.des_lock);

	for (chn = 0; chn < 4; chn++)
	{
		if ((nuc970_crdev.des_channels & (1 << chn)) == 0)
		{
			nuc970_crdev.des_channels |= (1 << chn);
			break;
		}
	}	
	if (chn >= 4)
	{
		spin_unlock(&nuc970_crdev.des_lock);
		return -ENOMEM;
	}
	
	ctx->mode = cryp_alg->algomode;
	ctx->channel = chn;
	ctx->tdes_ctrl = 0;
	ctx->tdes_regs = (struct nuc970_tdes_regs *)((u32)nuc970_crdev.regs + 0x208 + (0x40 * chn));
	ctx->is_first_block = 1;

	spin_unlock(&nuc970_crdev.des_lock);

	//printk("[%s],ctx=0x%x, chn=%d\n", __func__, (int)ctx, ctx->channel);

	return 0;
}

static void nuc970_des_exit(struct crypto_tfm *tfm)
{
	struct nuc970_ctx  *ctx = crypto_tfm_ctx(tfm);

	//printk("[%s]\n", __func__);

	spin_lock(&nuc970_crdev.des_lock);
	nuc970_crdev.des_channels &= ~(1 << ctx->channel);
	nuc970_crdev.regs->CRPT_TDES_CTL = TDES_STOP;
	spin_unlock(&nuc970_crdev.des_lock);
}


static struct cryp_algo_template nuc970_crypto_algs[] = {
	{
		.algomode = AES_ECB_MODE,
		.crypto = {
			.cra_name = "ecb(aes)",
			.cra_driver_name = "ecb-aes-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_aes_init,
			.cra_exit = nuc970_aes_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = AES_MIN_KEY_SIZE,
					.max_keysize = AES_MAX_KEY_SIZE,
					.setkey = nuc970_aes_setkey,
					.ivsize	= AES_BLOCK_SIZE,
					.encrypt = nuc970_aes_encrypt,
					.decrypt = nuc970_aes_decrypt,
				}
			}
		}
	},
	{
		.algomode = AES_CBC_MODE,
		.crypto = {
			.cra_name = "cbc(aes)",
			.cra_driver_name = "cbc-aes-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_aes_init,
			.cra_exit = nuc970_aes_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = AES_MIN_KEY_SIZE,
					.max_keysize = AES_MAX_KEY_SIZE,
					.setkey = nuc970_aes_setkey,
					.ivsize	= AES_BLOCK_SIZE,
					.encrypt = nuc970_aes_encrypt,
					.decrypt = nuc970_aes_decrypt,
				}
			}
		}
	},
	{
		.algomode = AES_CFB_MODE,
		.crypto = {
			.cra_name = "cfb(aes)",
			.cra_driver_name = "cfb-aes-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_aes_init,
			.cra_exit = nuc970_aes_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = AES_MIN_KEY_SIZE,
					.max_keysize = AES_MAX_KEY_SIZE,
					.ivsize	= AES_BLOCK_SIZE,
					.setkey = nuc970_aes_setkey,
					.encrypt = nuc970_aes_encrypt,
					.decrypt = nuc970_aes_decrypt,
				}
			}
		}
	},
	{
		.algomode = AES_OFB_MODE,
		.crypto = {
			.cra_name = "ofb(aes)",
			.cra_driver_name = "ofb-aes-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_aes_init,
			.cra_exit = nuc970_aes_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = AES_MIN_KEY_SIZE,
					.max_keysize = AES_MAX_KEY_SIZE,
					.setkey = nuc970_aes_setkey,
					.ivsize	= AES_BLOCK_SIZE,
					.encrypt = nuc970_aes_encrypt,
					.decrypt = nuc970_aes_decrypt,
				}
			}
		}
	},
	{
		.algomode = AES_CTR_MODE,
		.crypto = {
			.cra_name = "ctr(aes)",
			.cra_driver_name = "ctr-aes-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_aes_init,
			.cra_exit = nuc970_aes_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = AES_MIN_KEY_SIZE,
					.max_keysize = AES_MAX_KEY_SIZE,
					.setkey = nuc970_aes_setkey,
					.ivsize	= AES_BLOCK_SIZE,
					.encrypt = nuc970_aes_encrypt,
					.decrypt = nuc970_aes_decrypt,
				}
			}
		}
	},
	{
		.algomode = AES_CBCCS1_MODE,
		.crypto = {
			.cra_name = "cbc-cs1(aes)",
			.cra_driver_name = "cbc-cs1-aes-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_aes_init,
			.cra_exit = nuc970_aes_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = AES_MIN_KEY_SIZE,
					.max_keysize = AES_MAX_KEY_SIZE,
					.setkey = nuc970_aes_setkey,
					.ivsize	= AES_BLOCK_SIZE,
					.encrypt = nuc970_aes_encrypt,
					.decrypt = nuc970_aes_decrypt,
				}
			}
		}
	},
	{
		.algomode = AES_CBCCS2_MODE,
		.crypto = {
			.cra_name = "cbc-cs2(aes)",
			.cra_driver_name = "cbc-cs2-aes-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_aes_init,
			.cra_exit = nuc970_aes_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = AES_MIN_KEY_SIZE,
					.max_keysize = AES_MAX_KEY_SIZE,
					.setkey = nuc970_aes_setkey,
					.ivsize	= AES_BLOCK_SIZE,
					.encrypt = nuc970_aes_encrypt,
					.decrypt = nuc970_aes_decrypt,
				}
			}
		}
	},
	{
		.algomode = AES_CBCCS3_MODE,
		.crypto = {
			.cra_name = "cbc-cs3(aes)",
			.cra_driver_name = "cbc-cs3-aes-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_aes_init,
			.cra_exit = nuc970_aes_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = AES_MIN_KEY_SIZE,
					.max_keysize = AES_MAX_KEY_SIZE,
					.setkey = nuc970_aes_setkey,
					.ivsize	= AES_BLOCK_SIZE,
					.encrypt = nuc970_aes_encrypt,
					.decrypt = nuc970_aes_decrypt,
				}
			}
		}
	},
	{
		.algomode = TDES_ECB_MODE,
		.crypto = {
			.cra_name = "ecb(des)",
			.cra_driver_name = "ecb-des-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_des_init,
			.cra_exit = nuc970_des_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = DES_KEY_SIZE,
					.max_keysize = DES_KEY_SIZE,
					.setkey = nuc970_des_setkey,
					.ivsize	= DES_BLOCK_SIZE,
					.encrypt = nuc970_des_encrypt,
					.decrypt = nuc970_des_decrypt,
				}
			}
		}
	},
	{
		.algomode = TDES_CBC_MODE,
		.crypto = {
			.cra_name = "cbc(des)",
			.cra_driver_name = "cbc-des-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_des_init,
			.cra_exit = nuc970_des_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = DES_KEY_SIZE,
					.max_keysize = DES_KEY_SIZE,
					.setkey = nuc970_des_setkey,
					.ivsize	= DES_BLOCK_SIZE,
					.encrypt = nuc970_des_encrypt,
					.decrypt = nuc970_des_decrypt,
				}
			}
		}
	},
	{
		.algomode = TDES_CFB_MODE,
		.crypto = {
			.cra_name = "cfb(des)",
			.cra_driver_name = "cfb-des-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_des_init,
			.cra_exit = nuc970_des_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = DES_KEY_SIZE,
					.max_keysize = DES_KEY_SIZE,
					.setkey = nuc970_des_setkey,
					.ivsize	= DES_BLOCK_SIZE,
					.encrypt = nuc970_des_encrypt,
					.decrypt = nuc970_des_decrypt,
				}
			}
		}
	},
	{
		.algomode = TDES_OFB_MODE,
		.crypto = {
			.cra_name = "ofb(des)",
			.cra_driver_name = "ofb-des-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_des_init,
			.cra_exit = nuc970_des_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = DES_KEY_SIZE,
					.max_keysize = DES_KEY_SIZE,
					.setkey = nuc970_des_setkey,
					.ivsize	= DES_BLOCK_SIZE,
					.encrypt = nuc970_des_encrypt,
					.decrypt = nuc970_des_decrypt,
				}
			}
		}
	},
	{
		.algomode = TDES_CTR_MODE,
		.crypto = {
			.cra_name = "ctr(des)",
			.cra_driver_name = "ctr-des-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_des_init,
			.cra_exit = nuc970_des_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = DES_KEY_SIZE,
					.max_keysize = DES_KEY_SIZE,
					.setkey = nuc970_des_setkey,
					.ivsize	= DES_BLOCK_SIZE,
					.encrypt = nuc970_des_encrypt,
					.decrypt = nuc970_des_decrypt,
				}
			}
		}
	},
	{
		.algomode = TDES_ECB_MODE,
		.crypto = {
			.cra_name = "ecb(3des)",
			.cra_driver_name = "ecb-3des-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_des_init,
			.cra_exit = nuc970_des_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = 3 * DES_KEY_SIZE,
					.max_keysize = 3 * DES_KEY_SIZE,
					.setkey = nuc970_des_setkey,
					.ivsize	= DES_BLOCK_SIZE,
					.encrypt = nuc970_des_encrypt,
					.decrypt = nuc970_des_decrypt,
				}
			}
		}
	},
	{
		.algomode = TDES_CBC_MODE,
		.crypto = {
			.cra_name = "cbc(3des)",
			.cra_driver_name = "cbc-3des-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_des_init,
			.cra_exit = nuc970_des_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = 3 * DES_KEY_SIZE,
					.max_keysize = 3 * DES_KEY_SIZE,
					.setkey = nuc970_des_setkey,
					.ivsize	= DES_BLOCK_SIZE,
					.encrypt = nuc970_des_encrypt,
					.decrypt = nuc970_des_decrypt,
				}
			}
		}
	},
	{
		.algomode = TDES_CFB_MODE,
		.crypto = {
			.cra_name = "cfb(3des)",
			.cra_driver_name = "cfb-3des-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_des_init,
			.cra_exit = nuc970_des_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = 3 * DES_KEY_SIZE,
					.max_keysize = 3 * DES_KEY_SIZE,
					.setkey = nuc970_des_setkey,
					.ivsize	= DES_BLOCK_SIZE,
					.encrypt = nuc970_des_encrypt,
					.decrypt = nuc970_des_decrypt,
				}
			}
		}
	},
	{
		.algomode = TDES_OFB_MODE,
		.crypto = {
			.cra_name = "ofb(3des)",
			.cra_driver_name = "ofb-3des-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_des_init,
			.cra_exit = nuc970_des_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = 3 * DES_KEY_SIZE,
					.max_keysize = 3 * DES_KEY_SIZE,
					.setkey = nuc970_des_setkey,
					.ivsize	= DES_BLOCK_SIZE,
					.encrypt = nuc970_des_encrypt,
					.decrypt = nuc970_des_decrypt,
				}
			}
		}
	},
	{
		.algomode = TDES_CTR_MODE,
		.crypto = {
			.cra_name = "ctr(3des)",
			.cra_driver_name = "ctr-3des-nuc970",
			.cra_priority =	300,
			.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct nuc970_ctx),
			.cra_alignmask = 0xf,
			.cra_type = &crypto_ablkcipher_type,
			.cra_init = nuc970_des_init,
			.cra_exit = nuc970_des_exit,
			.cra_module = THIS_MODULE,
			.cra_u = {
				.ablkcipher = {
					.min_keysize = 3 * DES_KEY_SIZE,
					.max_keysize = 3 * DES_KEY_SIZE,
					.setkey = nuc970_des_setkey,
					.ivsize	= DES_BLOCK_SIZE,
					.encrypt = nuc970_des_encrypt,
					.decrypt = nuc970_des_decrypt,
				}
			}
		}
	},
};



/*******************************************************************************/
/*******************************************************************************/
/*******************************************************************************/


/*---------------------------------------------------------------------*/
/*                                                                     */
/*        NUC970 SHA/HMAC driver                                       */
/*                                                                     */
/*---------------------------------------------------------------------*/

#define SHA_BUFFER_LEN		PAGE_SIZE


void  nuc970_dump_digest()
{
	struct nuc970_crypto_regs  *crpt_regs = nuc970_crdev.regs;
	int  i;
	
	printk("DIGEST: ");
	for (i = 0; i < 8; i++)
		printk("0x%x\n", crpt_regs->CRPT_HMAC_DGST[i]);
	printk("\n");
}


static int do_sha(struct ahash_request *req, int is_last)
{
	struct nuc970_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct scatterlist   *in_sg;
	struct nuc970_crypto_regs  *crpt_regs = nuc970_crdev.regs;
	int  req_len, dma_len;
	int  timeout = 100000;

	in_sg = req->src;
	req_len = req->nbytes;
	
	while ((req_len > 0) && in_sg)
	{
		dma_len = min(in_sg->length, req_len);
		
		crpt_regs->CRPT_HMAC_DMACNT = dma_len;
		crpt_regs->CRPT_HMAC_SADDR = nuc970_crdev.hmac_inbuf_dma_addr;

		if (sg_copy_to_buffer(in_sg, 1, nuc970_crdev.hmac_inbuf, dma_len) != dma_len)
		{
			printk("sg in buffer error!\n");
			break;
		}	
		
		in_sg = sg_next(in_sg);
		
		if (dma_len == req_len)
			crpt_regs->CRPT_HMAC_CTL |= HMAC_DMALAST;
		
		crpt_regs->CRPT_HMAC_CTL |= HMAC_START | HMAC_DMAEN;
		
		while ((crpt_regs->CRPT_HMAC_STS & HMAC_BUSY) && timeout--)
		{
			cpu_relax();
		}
		
		if (timeout == 0)
		{
			printk("Crypto SHA/HMAC engine failed!\n");
			return 1;
		}
		
		/* For last DMA, copy the last byte to first bytes position */
		if (ctx->is_first_block)
		{
			ctx->is_first_block = 0;
			*(u8 *)(nuc970_crdev.hmac_inbuf) = *((u8 *)nuc970_crdev.hmac_inbuf + dma_len - 1);
		}
		else
		{
			*(u8 *)(nuc970_crdev.hmac_inbuf) = *((u8 *)nuc970_crdev.hmac_inbuf + dma_len);
		}
		
		req_len -= 	dma_len;
		
		//nuc970_dump_digest();
	}
	return 0;
}


static int nuc970_sha_update(struct ahash_request *req)
{
	//printk("nuc970_sha_update - %d bytes\n", req->nbytes);
	return do_sha(req, 0);
}

static int nuc970_sha_final(struct ahash_request *req)
{
	struct nuc970_crypto_regs  *crpt_regs = nuc970_crdev.regs;

	//printk("nuc970_sha_final - %d bytes\n", req->nbytes);

	do_sha(req, 1);

	if ((crpt_regs->CRPT_HMAC_CTL & HMAC_OPMODE_MASK) == HMAC_SHA1)
		memcpy(req->result, (u8 *)&(crpt_regs->CRPT_HMAC_DGST[0]), SHA1_DIGEST_SIZE);
	else if ((crpt_regs->CRPT_HMAC_CTL & HMAC_OPMODE_MASK) == HMAC_SHA224)
		memcpy(req->result, (u8 *)&(crpt_regs->CRPT_HMAC_DGST[0]), SHA224_DIGEST_SIZE);
	else if ((crpt_regs->CRPT_HMAC_CTL & HMAC_OPMODE_MASK) == HMAC_SHA256)
		memcpy(req->result, (u8 *)&(crpt_regs->CRPT_HMAC_DGST[0]), SHA256_DIGEST_SIZE);
	else if ((crpt_regs->CRPT_HMAC_CTL & HMAC_OPMODE_MASK) == HMAC_SHA384)
		memcpy(req->result, (u8 *)&(crpt_regs->CRPT_HMAC_DGST[0]), SHA384_DIGEST_SIZE);
	else
		memcpy(req->result, (u8 *)&(crpt_regs->CRPT_HMAC_DGST[0]), SHA512_DIGEST_SIZE);

	return 0;
}

static int nuc970_sha_init(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct nuc970_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct nuc970_crypto_regs  *crpt_regs = nuc970_crdev.regs;
	
	crpt_regs->CRPT_HMAC_CTL = HMAC_STOP;
	//printk("nuc970_sha_init: digest size: %d\n", crypto_ahash_digestsize(tfm));
	crpt_regs->CRPT_HMAC_CTL = HMAC_INSWAP | HMAC_OUTSWAP;
	crpt_regs->CRPT_HMAC_KEYCNT = 0;
	ctx->is_first_block = 1;
	ctx->sha_remain_bytes = 0;

	switch (crypto_ahash_digestsize(tfm)) 
	{
		case SHA1_DIGEST_SIZE:
			crpt_regs->CRPT_HMAC_CTL |= HMAC_SHA1;
			break;

		case SHA224_DIGEST_SIZE:
			crpt_regs->CRPT_HMAC_CTL |= HMAC_SHA224;
			break;

		case SHA256_DIGEST_SIZE:
			crpt_regs->CRPT_HMAC_CTL |= HMAC_SHA256;
			break;

		case SHA384_DIGEST_SIZE:
			crpt_regs->CRPT_HMAC_CTL |= HMAC_SHA384;
			break;
			
		case SHA512_DIGEST_SIZE:
			crpt_regs->CRPT_HMAC_CTL |= HMAC_SHA512;
			break;

		default:
			return -EINVAL;
			break;
	}

	return 0;
}

static int nuc970_sha_finup(struct ahash_request *req)
{
	int err1, err2;

	//printk("nuc970_sha_finup.\n");

	err1 = nuc970_sha_update(req);
	if (err1 == -EINPROGRESS || err1 == -EBUSY)
		return err1;

	/*
	 * final() has to be always called to cleanup resources
	 * even if udpate() failed, except EINPROGRESS
	 */
	err2 = nuc970_sha_final(req);

	return err1 ?: err2;
}

static int nuc970_sha_digest(struct ahash_request *req)
{
	//printk("nuc970_sha_digest.\n");
	return nuc970_sha_init(req) ?: nuc970_sha_finup(req);
}

static int nuc970_sha_cra_init(struct crypto_tfm *tfm)
{
	//printk("nuc970_sha_cra_init.\n");

	//spin_lock(&nuc970_crdev.sha_lock);
	return 0;
}

static void nuc970_sha_cra_exit(struct crypto_tfm *tfm)
{
	//printk("nuc970_sha_cra_exit.\n");
	//spin_unlock(&nuc970_crdev.sha_lock);
}


static struct ahash_alg nuc970_hash_algs[] = {
{
	.init		= nuc970_sha_init,
	.update		= nuc970_sha_update,
	.final		= nuc970_sha_final,
	.finup		= nuc970_sha_finup,
	.digest		= nuc970_sha_digest,
	.halg = {
		.digestsize	= SHA1_DIGEST_SIZE,
		.base	= {
			.cra_name		= "sha1",
			.cra_driver_name	= "nuc970-sha1",
			.cra_priority	= 100,
			.cra_flags		= CRYPTO_ALG_TYPE_AHASH | CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK,
			.cra_blocksize	= SHA1_BLOCK_SIZE,
			.cra_ctxsize	= sizeof(struct nuc970_ctx),
			.cra_alignmask	= 0,
			.cra_module		= THIS_MODULE,
			.cra_init		= nuc970_sha_cra_init,
			.cra_exit		= nuc970_sha_cra_exit,
		}
	}
},
};

static void nuc970_crypto_remove(struct platform_device *pdev)
{
	int  i;
	struct device *dev = &pdev->dev;
	struct resource *res;

	for (i = 0; i < ARRAY_SIZE(nuc970_crypto_algs); i++) 
		crypto_unregister_alg(&nuc970_crypto_algs[i].crypto);

	for (i = 0; i < ARRAY_SIZE(nuc970_hash_algs); i++)
		crypto_unregister_ahash(&nuc970_hash_algs[i]);

	dma_free_coherent(dev, DMA_BUFSZ, nuc970_crdev.aes_inbuf, nuc970_crdev.aes_inbuf_dma_addr);
	dma_free_coherent(dev, DMA_BUFSZ, nuc970_crdev.aes_outbuf, nuc970_crdev.aes_outbuf_dma_addr);
	dma_free_coherent(dev, DMA_BUFSZ, nuc970_crdev.des_inbuf, nuc970_crdev.des_inbuf_dma_addr);
	dma_free_coherent(dev, DMA_BUFSZ, nuc970_crdev.des_outbuf, nuc970_crdev.des_outbuf_dma_addr);
	dma_free_coherent(dev, DMA_BUFSZ, nuc970_crdev.hmac_inbuf, nuc970_crdev.hmac_inbuf_dma_addr);

	iounmap(nuc970_crdev.regs);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	release_mem_region(res->start, resource_size(res));
}

static int nuc970_crypto_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	int   i, err, ret;

    if (IS_ERR(clk_get(NULL, "crypto_hclk"))) {
        printk("nuc970_crypto_probe clk_get error!!\n");
        return -1;
    }

	/* Enable Cryptographic Accerlator clock */
    clk_prepare(clk_get(NULL, "crypto_hclk"));	
    clk_enable(clk_get(NULL, "crypto_hclk"));
	
	memset((u8 *)&nuc970_crdev, 0, sizeof(nuc970_crdev));

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENXIO;

	if (!request_mem_region(res->start, resource_size(res), pdev->name))
		return -EBUSY;

	nuc970_crdev.regs = ioremap(res->start, resource_size(res));
	if (!nuc970_crdev.regs)
		return -ENOMEM;
		
	//printk("nuc970_crdev.regs = 0x%x\n", (u32)nuc970_crdev.regs);

	nuc970_crdev.aes_inbuf = dma_alloc_coherent(dev, DMA_BUFSZ, &nuc970_crdev.aes_inbuf_dma_addr, GFP_KERNEL);
	nuc970_crdev.aes_outbuf = dma_alloc_coherent(dev, DMA_BUFSZ, &nuc970_crdev.aes_outbuf_dma_addr, GFP_KERNEL);
	nuc970_crdev.des_inbuf = dma_alloc_coherent(dev, DMA_BUFSZ, &nuc970_crdev.des_inbuf_dma_addr, GFP_KERNEL);
	nuc970_crdev.des_outbuf = dma_alloc_coherent(dev, DMA_BUFSZ, &nuc970_crdev.des_outbuf_dma_addr, GFP_KERNEL);
	nuc970_crdev.hmac_inbuf = dma_alloc_coherent(dev, DMA_BUFSZ, &nuc970_crdev.hmac_inbuf_dma_addr, GFP_KERNEL);
	
	if (!nuc970_crdev.aes_inbuf || !nuc970_crdev.aes_outbuf || !nuc970_crdev.des_inbuf || 
		!nuc970_crdev.des_outbuf || !nuc970_crdev.hmac_inbuf)
	{
		ret = -ENOMEM;
		goto failed;
	}

	for (i = 0; i < ARRAY_SIZE(nuc970_crypto_algs); i++) 
	{
		err = crypto_register_alg(&nuc970_crypto_algs[i].crypto);
		if (err)
			goto failed;
	}

	for (i = 0; i < ARRAY_SIZE(nuc970_hash_algs); i++)
	{
		err = crypto_register_ahash(&nuc970_hash_algs[i]);
		if (err)
			goto failed;
	}
	
	spin_lock_init(&nuc970_crdev.aes_lock);
	spin_lock_init(&nuc970_crdev.des_lock);
	spin_lock_init(&nuc970_crdev.sha_lock);
		
	printk(KERN_NOTICE "NUC970 Crypto engine enabled.\n");
	return 0;

failed:

	if (nuc970_crdev.aes_inbuf)
		dma_free_coherent(dev, DMA_BUFSZ, nuc970_crdev.aes_inbuf, nuc970_crdev.aes_inbuf_dma_addr);
	if (nuc970_crdev.aes_outbuf)
		dma_free_coherent(dev, DMA_BUFSZ, nuc970_crdev.aes_outbuf, nuc970_crdev.aes_outbuf_dma_addr);
	if (nuc970_crdev.des_inbuf)
		dma_free_coherent(dev, DMA_BUFSZ, nuc970_crdev.des_inbuf, nuc970_crdev.des_inbuf_dma_addr);
	if (nuc970_crdev.des_outbuf)
		dma_free_coherent(dev, DMA_BUFSZ, nuc970_crdev.des_outbuf, nuc970_crdev.des_outbuf_dma_addr);
	if (nuc970_crdev.hmac_inbuf)
		dma_free_coherent(dev, DMA_BUFSZ, nuc970_crdev.hmac_inbuf, nuc970_crdev.hmac_inbuf_dma_addr);

	iounmap(nuc970_crdev.regs);
	release_mem_region(res->start, resource_size(res));

	printk("NUC970 Crypto initialization failed.\n");
	return ret;
}


static struct platform_driver nuc970_crypto_driver = {
	.probe		= nuc970_crypto_probe,
	.remove		= nuc970_crypto_remove,
	.driver		= {
		.name	= "nuc970-crypto",
		.owner	= THIS_MODULE,
	},
};

module_platform_driver(nuc970_crypto_driver);

MODULE_AUTHOR("Nuvoton Technology Corporation");
MODULE_DESCRIPTION("NUC970 Cryptographic Accerlerator");
MODULE_LICENSE("GPL");