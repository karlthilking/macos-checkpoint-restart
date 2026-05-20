/* pac.c */
#define _XOPEN_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <ucontext.h>
#include "types.h"
#include "pac.h"

/**
 * pac_patch_ucontext:
 *  Patch ucontext pointer that was delivered in signal
 *  handler frame during checkpoint.
 */
void pac_patch_ucontext(ucontext_t *ucp)
{
        u64 fp = get_ucontext_fp(ucp);
        u64 lr = get_ucontext_lr(ucp);
        u64 sp = get_ucontext_sp(ucp);
        
        /**
         * Unconditionally strip and resign link register
         * with constant discriminator used for auth in
         * _setcontext.
         *
         * By setting thread state flags to 0, _setcontext
         * will take the path of authenticating the link
         * register against its own discriminator, and then
         * will manually re-sign the link register with the
         * correct stack pointer value.
         */
        XPACI(lr);
        PACIA(lr, LR_DISCRIMINATOR);
        set_ucontext_lr(ucp, lr);
        set_ucontext_flags(ucp, 0);
        
        XPACD(fp);
        PACDA(fp, FP_DISCRIMINATOR);
        set_ucontext_fp(ucp, fp);
        
        XPACD(sp);
        PACDA(sp, SP_DISCRIMINATOR);
        set_ucontext_sp(ucp, sp);

        /**
         * Copy the context pointed to by ucp->uc_mcontext
         * to ucp->__mcontext_data because setcontext()
         * will access the register context through
         * __mcontext_data.
         */
        memcpy(&ucp->__mcontext_data, ucp->uc_mcontext,
               sizeof(ucp->__mcontext_data));
}

int pac_strip_context(ckpt_context_t *ctx)
{
        u64 lr = get_ucontext_lr(&ctx->uc);

        if (PTRAUTH_SIGNED(lr)) {
                ctx->bitmap |= (1ull << ARM64_LR);
                
                /* Remember original signed value */
                u64 old = lr;

                u64 uctx_sp = get_ucontext_sp(&ctx->uc);
                u64 prev_sp = get_ucontext_fp(&ctx->uc) + 0x10;
                
                /**
                 * Strip signature and set context to link register to
                 * stripped link register value
                 */
                XPACI(lr);
                set_ucontext_lr(&ctx->uc, lr);

                u64 getcontext_pacibsp  = lr;
                u64 prev_pacibsp        = lr;
                u64 const_pacia         = lr;

                XPACD(uctx_sp);
                XPACD(prev_sp);
                
                PACIB(getcontext_pacibsp, uctx_sp);
                PACIB(prev_pacibsp, prev_sp);
                PACIA(const_pacia, LR_DISCRIMINATOR);

                /**
                 * Compare reproduced signatures against the original
                 * signed link register value to determine the modifier
                 * used when the link register was originally signed.
                 */
                if (getcontext_pacibsp == old)
                        ctx->modifiers[ARM64_LR] = uctx_sp;
                else if (prev_pacibsp == old)
                        ctx->modifiers[ARM64_LR] = prev_sp;
                else if (const_pacia == old)
                        ctx->modifiers[ARM64_LR] = LR_DISCRIMINATOR;
                else {
                        fprintf(stderr,
                                "Failed to identify modifier used to "
                                "sign link register (lr=0x%llx)\n", old);
                        return -1;
                }
        }

        if (PTRAUTH_SIGNED(get_ucontext_fp(&ctx->uc))) {
                u64 old         = get_ucontext_fp(&ctx->uc);
                u64 check       = old;

                XPACD(check);
                PACDA(check, FP_DISCRIMINATOR);
                assert(check == old);

                ctx->bitmap |= (1ull << ARM64_FP);
                ctx->modifiers[ARM64_FP] = FP_DISCRIMINATOR;
                
                XPACD(old);
                set_ucontext_fp(&ctx->uc, old);
        }

        if (PTRAUTH_SIGNED(get_ucontext_sp(&ctx->uc))) {
                u64 old         = get_ucontext_sp(&ctx->uc);
                u64 check       = old;

                XPACD(check);
                PACDA(check, SP_DISCRIMINATOR);
                assert(check == old);

                ctx->bitmap |= (1ull << ARM64_SP);
                ctx->modifiers[ARM64_SP] = SP_DISCRIMINATOR;

                XPACD(old);
                set_ucontext_sp(&ctx->uc, old);
        }

        return 0;
}

void pac_sign_context(ckpt_context_t *ctx)
{
        u64 lr = get_ucontext_lr(&ctx->uc);

        assert(!PTRAUTH_SIGNED(lr));
        if (ctx->bitmap & (1ull << ARM64_LR)) {
                u64 mod = ctx->modifiers[ARM64_LR];
                assert(!PTRAUTH_SIGNED(mod));

                if (mod == LR_DISCRIMINATOR) {
                        PACIA(lr, LR_DISCRIMINATOR);
                        set_ucontext_flags(&ctx->uc, 0);
                } else {
                        PACIB(lr, mod);
                        set_ucontext_flags(&ctx->uc, LR_SIGNED_WITH_IB);
                }
                set_ucontext_lr(&ctx->uc, lr);
        } else {
                /**
                 * Even if it was recorded that the link register was not
                 * signed, setcontext will authenticate the restored
                 * link register value with its constant discriminator,
                 * so the link register should be signed with the IA key
                 * and constant discriminator.
                 */
                PACIA(lr, LR_DISCRIMINATOR);
                set_ucontext_lr(&ctx->uc, lr);
                set_ucontext_flags(&ctx->uc, 0);
        }

        u64 sp = get_ucontext_sp(&ctx->uc);
        // assert(ctx->modifiers[ARM64_SP] == SP_DISCRIMINATOR);
        PACDA(sp, SP_DISCRIMINATOR);
        set_ucontext_sp(&ctx->uc, sp);

        u64 fp = get_ucontext_fp(&ctx->uc);
        // assert(ctx->modifiers[ARM64_FP] == FP_DISCRIMINATOR);
        PACDA(fp, FP_DISCRIMINATOR);
        set_ucontext_fp(&ctx->uc, fp);
}

u32 pac_strip_frames(ckpt_callframe_t *frames, u64 *fp)
{
        ckpt_callframe_t        *cf = frames;
        u64                     *lr, sp, prev_fp, old, check;
        u32                     nr_saved = 0;

        for_each_signed_frame(fp) {
                prev_fp = fp[0];
                lr      = fp + 1;
                sp      = (u64)fp + 0x10;

                assert(PTRAUTH_SIGNED(*lr));
                
                old     = *lr;
                check   = *lr;

                /**
                 * Verify that link register was signed with pacibsp
                 * (IB key + stack pointer at function entry) by
                 * reproducing the signed link register value.
                 */
                XPACI(check);
                PACIB(check, sp);
                assert(check == old);

                XPACI(*lr);
                cf->fp  = prev_fp;
                cf->lr  = *lr;

                assert(PAC_IB_KEY > 0 && PAC_IB_KEY <= 3);
                FRAME_SET_FLAGS(cf, 1, PAC_IB_KEY, ARM64_SP);
                
                cf++;
                nr_saved++;
        }

        return nr_saved;
}

void pac_sign_frames(const ckpt_callframe_t *frames, 
                     u64 *fp, const u32 nr_frames)
{
        const ckpt_callframe_t  *cf = frames;
        u64                     *lr, sp, prev_fp, check;
        u32                     nr_signed = 0;

        for_each_frame(fp) {
                if (nr_signed == nr_frames)
                        break;

                prev_fp = fp[0];
                while (prev_fp != cf->fp) {
                        assert(prev_fp != 0);
                        fp      = (u64 *)prev_fp;
                        prev_fp = fp[0];
                }
                
                /**
                 * +---------------------+ <-- stack pointer at entry
                 * | saved link register |
                 * +---------------------+
                 * | saved frame pointer |
                 * +---------------------+ <-- current frame pointer
                 */
                lr = fp + 1;
                sp = (u64)fp + 0x10;
                
                assert(cf->lr == *lr && FRAME_LR_SIGNED(cf) &&
                       FRAME_PAC_KEY(cf) == PAC_IB_KEY &&
                       FRAME_PAC_MODIFIER(cf) == ARM64_SP);
                
                /* Sign link register with IB key and sp at entry */
                PACIB(*lr, sp);
                
                /**
                 * Verify that authenticating signed link register value
                 * with stack pointer at function entry succeeds
                 */
                check = *lr;
                AUTIB(check, sp);
                assert(PTRAUTH_SIGNED(*lr) && !PTRAUTH_FAILED(check));

                cf++;
                nr_signed++;
        }
}

void pac_check()
{
        int     err = 0;
        u64     data_ptr, instr_ptr, mod = 0ull;

        asm volatile(
                "mov x9, sp     \n"
                "mov x10, x30   \n"
                "xpacd x9       \n"
                "xpaci x10      \n"
                "mov %0, x9     \n"
                "mov %1, x10    \n"
                : "=r" (data_ptr), "=r" (instr_ptr)
                :
                : "memory"
        );

        /* Test instruction address pac and aut instructions */
        PACIA(instr_ptr, mod);
        if (PTRAUTH_SIGNED(instr_ptr)) {
                AUTIA(instr_ptr, mod);
                assert(!PTRAUTH_SIGNED(instr_ptr));
        } else {
                fprintf(stderr, "pacia noop\n");
                err = 1;
        }

        PACIB(instr_ptr, mod);
        if (PTRAUTH_SIGNED(instr_ptr)) {
                AUTIB(instr_ptr, mod);
                assert(!PTRAUTH_SIGNED(instr_ptr));
        } else {
                fprintf(stderr, "pacib noop\n");
                err = 1;
        }

        /* Test data address pac and aut instructions */
        PACDA(data_ptr, mod);
        if (PTRAUTH_SIGNED(data_ptr)) {
                AUTDA(data_ptr, mod);
                assert(!PTRAUTH_SIGNED(data_ptr));
        } else {
                fprintf(stderr, "pacda noop\n");
                err = 1;
        }

        PACDB(data_ptr, mod);
        if (PTRAUTH_SIGNED(data_ptr)) {
                AUTDB(data_ptr, mod);
                assert(!PTRAUTH_SIGNED(data_ptr));
        } else {
                fprintf(stderr, "pacdb noop\n");
                err = 1;
        }

        if (err)
                __builtin_trap();
}
