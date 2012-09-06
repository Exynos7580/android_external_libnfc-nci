/********************************************************************************
**                                                                              *
**  Name        nfc_prm_brcm.c                                                  *
**                                                                              *
**  Function    this file contains patch ram loader API source code             *
**                                                                              *
**                                                                              *
**  Copyright (c) 2012, Broadcom Inc., All Rights Reserved.                     *
**  Proprietary and confidential.                                               *
**                                                                              *
*********************************************************************************/
#include "nfc_target.h"
#include "hcidefs.h"
#include "gki.h"
#include "nfc_api.h"
#include "nfc_brcm_api.h"
#include "nfc_brcm_int.h"
#include "nci_hmsgs.h"
#include "nfc_int.h"
#include "nci_int.h"
#include <string.h>

#if (NFC_BRCM_VS_INCLUDED == TRUE)

/*****************************************************************************
* Definitions
*****************************************************************************/

/* Internal flags */
#define BRCM_PRM_FLAGS_USE_PATCHRAM_BUF  0x01    /* Application provided patchram in a single buffer */
#define BRCM_PRM_FLAGS_RFU               0x02    /* Reserved for future use */
#define BRCM_PRM_FLAGS_SIGNATURE_SENT    0x04    /* Signature sent to NFCC */
#define BRCM_PRM_FLAGS_I2C_FIX_REQUIRED  0x08    /* PreI2C patch required */
#define BRCM_PRM_FLAGS_NO_NVM            0x10    /* Not NVM available (patch downloaded to SRAM) */
#define BRCM_PRM_FLAGS_SUPPORT_RESET_NTF 0x20    /* Support RESET_NTF from NFCC after sending signature */
#define BRCM_PRM_FLAGS_NVM_FPM_CORRUPTED 0x40    /* FPM patch in NVM failed CRC check */
#define BRCM_PRM_FLAGS_NVM_LPM_CORRUPTED 0x80    /* LPM patch in NVM failed CRC check */

/* max patch data length (Can be overridden by platform for ACL HCI command size) */
#ifndef BRCM_PRM_HCD_CMD_MAXLEN
#define BRCM_PRM_HCD_CMD_MAXLEN  250
#endif

/* Secure patch download definitions */
#define BRCM_PRM_NCD_PATCHFILE_HDR_LEN  7       /* PRJID + MAJORVER + MINORVER + COUNT */
#define BRCM_PRM_NCD_PATCH_VERSION_LEN  16

#define BRCM_PRM_SPD_PRE_DOWNLOAD_DELAY (500)   /* Delay before starting to patch download (in ms) */

/* Enumeration of power modes IDs */
#define BRCM_PRM_SPD_POWER_MODE_LPM             0
#define BRCM_PRM_SPD_POWER_MODE_FPM             1

/* Set to TRUE to always download patch regardless of version */
#ifndef BRCM_PRM_SKIP_VERSION_CHECK
#define BRCM_PRM_SKIP_VERSION_CHECK     FALSE
#endif

/* Version string for BCM20791B3 */
const UINT8 BRCM_PRM_BCM20791B3_STR[]   = "20791B3";
#define BRCM_PRM_BCM20791B3_STR_LEN     (sizeof (BRCM_PRM_BCM20791B3_STR)-1)

/* Mininum payload size for SPD NCI commands (used to validate PRM_SetSpdNciCmdPayloadSize) */
/* Default is 32, as required by the NCI specifications; however this value may be          */
/* over-riden for platforms that have transport packet limitations                          */
#ifndef BRCM_PRM_MIN_NCI_CMD_PAYLOAD_SIZE
#define BRCM_PRM_MIN_NCI_CMD_PAYLOAD_SIZE   (32)
#endif

/* Vendor specified command complete callback */
void nfc_brcm_prm_nci_command_complete_cback (tNFC_VS_EVT event, UINT16 data_len, UINT8 *p_data);

#define BRCM_PRM_CMD_TOUT                    (1000) /* timeout for command (in ms)              */
#define BRCM_PRM_MINIDRV_2_PATCH_RAM_DELAY   (50)   /* delay after download mini driver (in ms) */
#define BRCM_PRM_END_DELAY                   (250)  /* delay before sending any new command (ms)*/

#ifndef BRCM_PRM_RESET_NTF_DELAY
#define BRCM_PRM_RESET_NTF_DELAY            (10000) /* amount of time to wait for RESET NTF after patch download */
#endif

#ifndef BRCM_PRM_POST_I2C_FIX_DELAY
#define BRCM_PRM_POST_I2C_FIX_DELAY         (500)   /* amount of time to wait after downloading preI2C patch before downloading LPM/FPM patch */
#endif

/* command to get currently downloaded patch version */
static UINT8 brcm_prm_get_patch_version_cmd [NCI_MSG_HDR_SIZE] =
{
    NCI_MTS_CMD|NCI_GID_PROP,
    NCI_MSG_GET_PATCH_VERSION,
    0x00
};


#ifndef BRCM_PRM_DEBUG
#define BRCM_PRM_DEBUG  TRUE
#endif

#if (BRCM_PRM_DEBUG == TRUE)
#define NFC_BRCM_PRM_STATE(str)  NCI_TRACE_DEBUG2 ("%s st: %d", str, nfc_brcm_cb.prm.state)
#else
#define NFC_BRCM_PRM_STATE(str)
#endif

void nfc_brcm_prm_post_baud_update (tNFC_STATUS status);

/*******************************************************************************
**
** Function         nfc_brcm_prm_spd_handle_download_complete
**
** Description      Patch download complete (for secure patch download)
**
** Returns          void
**
*******************************************************************************/
void nfc_brcm_prm_spd_handle_download_complete (UINT8 event)
{
    nfc_brcm_cb.prm.state = BRCM_PRM_ST_IDLE;

    /* Notify application now */
    if (nfc_brcm_cb.prm.p_cback)
        (nfc_brcm_cb.prm.p_cback) (event);
}

/*******************************************************************************
**
** Function         nfc_brcm_prm_spd_send_next_segment
**
** Description      Send next patch segment (for secure patch download)
**
** Returns          void
**
*******************************************************************************/
void nfc_brcm_prm_spd_send_next_segment (void)
{
    UINT8   *p_src;
    UINT16  len, offset = nfc_brcm_cb.prm.cur_patch_offset;
    UINT8   hcit, oid, hdr0, type;
    UINT8   chipverlen;
    UINT8   chipverstr[NCI_SPD_HEADER_CHIPVER_LEN];
    UINT8   patch_hdr_size = NCI_MSG_HDR_SIZE + 1; /* 1 is for HCIT */

    /* Validate that segment is at least big enought to have NCI_MSG_HDR_SIZE + 1 (hcit) */
    if (nfc_brcm_cb.prm.cur_patch_len_remaining < patch_hdr_size)
    {
        NCI_TRACE_ERROR0 ("Unexpected end of patch.");
        nfc_brcm_prm_spd_handle_download_complete (BRCM_PRM_ABORT_INVALID_PATCH_EVT);
        return;
    }

    /* Parse NCI command header */
    p_src = (UINT8*) (nfc_brcm_cb.prm.p_cur_patch_data + offset);
    STREAM_TO_UINT8 (hcit, p_src);
    STREAM_TO_UINT8 (hdr0, p_src);
    STREAM_TO_UINT8 (oid,  p_src);
    STREAM_TO_UINT8 (len,  p_src);
    STREAM_TO_UINT8 (type, p_src);


    /* Update number of bytes comsumed */
    nfc_brcm_cb.prm.cur_patch_offset += (len + patch_hdr_size);
    nfc_brcm_cb.prm.cur_patch_len_remaining -=  (len + patch_hdr_size);

    /* Check if sending signature byte */
    if (  (oid == NCI_MSG_SECURE_PATCH_DOWNLOAD )
        &&(type == NCI_SPD_TYPE_SIGNATURE)  )
    {
        nfc_brcm_cb.prm.flags |= BRCM_PRM_FLAGS_SIGNATURE_SENT;
    }
    /* Check for header */
    else if (  (oid == NCI_MSG_SECURE_PATCH_DOWNLOAD )
             &&(type == NCI_SPD_TYPE_HEADER)  )
    {
        /* Check if patch is for BCM20791B3 */
        p_src += NCI_SPD_HEADER_OFFSET_CHIPVERLEN;
        STREAM_TO_UINT8 (chipverlen, p_src);
        STREAM_TO_ARRAY (chipverstr, p_src, NCI_SPD_HEADER_CHIPVER_LEN);

        if (memcmp (BRCM_PRM_BCM20791B3_STR, chipverstr, BRCM_PRM_BCM20791B3_STR_LEN) == 0)
        {
            /* Patch is for BCM2079B3 - do not wait for RESET_NTF after patch download */
            nfc_brcm_cb.prm.flags &= ~BRCM_PRM_FLAGS_SUPPORT_RESET_NTF;
        }
        else
        {
            /* Patch is for BCM2079B4 or newer - wait for RESET_NTF after patch download */
            nfc_brcm_cb.prm.flags |= BRCM_PRM_FLAGS_SUPPORT_RESET_NTF;
        }
    }

    /* Send the command (not including HCIT here) */
    nci_brcm_send_nci_cmd ((UINT8*) (nfc_brcm_cb.prm.p_cur_patch_data + offset + 1), (UINT8) (len + NCI_MSG_HDR_SIZE),
                            nfc_brcm_prm_nci_command_complete_cback);
}

/*******************************************************************************
**
** Function         nfc_brcm_prm_spd_handle_next_patch_start
**
** Description      Handle start of next patch (for secure patch download)
**
** Returns          void
**
*******************************************************************************/
void nfc_brcm_prm_spd_handle_next_patch_start (void)
{
    UINT32  cur_patch_mask;
    UINT32  cur_patch_len;
    BOOLEAN found_patch_to_download = FALSE;

    while (!found_patch_to_download)
    {
        /* Get length of current patch */
        cur_patch_len = nfc_brcm_cb.prm.spd_patch_desc[nfc_brcm_cb.prm.spd_cur_patch_idx].len;

        /* Check if this is a patch we need to download */
        cur_patch_mask = ((UINT32) 1 << nfc_brcm_cb.prm.spd_patch_desc[nfc_brcm_cb.prm.spd_cur_patch_idx].power_mode);
        if (nfc_brcm_cb.prm.spd_patch_needed_mask & cur_patch_mask)
        {
            found_patch_to_download = TRUE;
        }
        else
        {
            /* Do not need to download this patch. Skip to next patch */
            NCI_TRACE_DEBUG1 ("Skipping patch for power_mode %i.", nfc_brcm_cb.prm.spd_patch_desc[nfc_brcm_cb.prm.spd_cur_patch_idx].power_mode);

            nfc_brcm_cb.prm.spd_cur_patch_idx++;
            if (nfc_brcm_cb.prm.spd_cur_patch_idx >= nfc_brcm_cb.prm.spd_patch_count)
            {
                /* No more to download */
                nfc_brcm_prm_spd_handle_download_complete (BRCM_PRM_COMPLETE_EVT);
                return;
            }
            else if (!(nfc_brcm_cb.prm.flags & BRCM_PRM_FLAGS_USE_PATCHRAM_BUF))
            {
                /* Notify adaptation layer to call PRM_DownloadContinue with the next patch header */
                (nfc_brcm_cb.prm.p_cback) (BRCM_PRM_SPD_GET_NEXT_PATCH);
                return;
            }
            else
            {
                /* Patch in buffer. Skip over current patch. Check next patch */
                nfc_brcm_cb.prm.cur_patch_len_remaining -= (UINT16) cur_patch_len;
                nfc_brcm_cb.prm.cur_patch_offset += (UINT16) cur_patch_len;
            }
        }
    }


    /* Begin downloading patch */
    NCI_TRACE_DEBUG1 ("Downloading patch for power_mode %i.", nfc_brcm_cb.prm.spd_patch_desc[nfc_brcm_cb.prm.spd_cur_patch_idx].power_mode);
    nfc_brcm_cb.prm.state = BRCM_PRM_ST_SPD_DOWNLOADING;
    nfc_brcm_prm_spd_send_next_segment ();
}

#if (defined (NFC_I2C_PATCH_INCLUDED) && (NFC_I2C_PATCH_INCLUDED == TRUE))
/*******************************************************************************
**
** Function         nfc_brcm_prm_spd_download_i2c_fix
**
** Description      Start downloading patch for i2c fix
**
** Returns          void
**
*******************************************************************************/
void nfc_brcm_prm_spd_download_i2c_fix (void)
{
    UINT8 *p, *p_start;
    UINT16 patchfile_project_id;
    UINT16 patchfile_ver_major;
    UINT16 patchfile_ver_minor;
    UINT16 patchfile_patchsize;
    UINT8 u8;

    NCI_TRACE_DEBUG0 ("Downloading I2C fix...");

    /* Save pointer and offset of patchfile, so we can resume after downloading the i2c fix */
    nfc_brcm_cb.prm.spd_patch_offset = nfc_brcm_cb.prm.cur_patch_offset;
    nfc_brcm_cb.prm.spd_patch_len_remaining = nfc_brcm_cb.prm.cur_patch_len_remaining;

    /* Initialize pointers for downloading i2c fix */
    nfc_brcm_cb.prm.p_cur_patch_data = nfc_brcm_cb.prm_i2c.p_patch;
    nfc_brcm_cb.prm.cur_patch_offset = 0;
    nfc_brcm_cb.prm.cur_patch_len_remaining = nfc_brcm_cb.prm_i2c.len;

    /* Parse the i2c patchfile */
    if (nfc_brcm_cb.prm.cur_patch_len_remaining >= BRCM_PRM_NCD_PATCHFILE_HDR_LEN)
    {
        /* Parse patchfile header */
        p = (UINT8 *) nfc_brcm_cb.prm.p_cur_patch_data;
        p_start = p;
        STREAM_TO_UINT16 (patchfile_project_id, p);
        STREAM_TO_UINT16 (patchfile_ver_major, p);
        STREAM_TO_UINT16 (patchfile_ver_minor, p);

        /* RFU */
        p++;

        /* Check how many patches are in the patch file */
        STREAM_TO_UINT8 (u8, p);

        /* Should only be one patch */
        if (u8 > 1)
        {
            NCI_TRACE_ERROR1 ("Invalid i2c fix: invalid number of patches (%i)", u8);
            nfc_brcm_prm_spd_handle_download_complete (BRCM_PRM_ABORT_INVALID_PATCH_EVT);
            return;
        }


        /* Get info about the i2c patch*/
        STREAM_TO_UINT8 (u8, p);                     /* power mode (not needed for i2c patch)    */
        STREAM_TO_UINT16 (patchfile_patchsize, p);   /* size of patch                            */

        /* 5 byte RFU */
        p += 5;

        /* Adjust length to exclude patchfiloe header */
        nfc_brcm_cb.prm.cur_patch_len_remaining -= (UINT16) (p - p_start);       /* Adjust size of patchfile                        */
        nfc_brcm_cb.prm.cur_patch_offset += (UINT16) (p - p_start);              /* Bytes of patchfile transmitted/processed so far */

        /* Begin sending patch to the NFCC */
        nfc_brcm_prm_spd_send_next_segment ();
    }
    else
    {
        /* ERROR: Bad length for patchfile */
        NCI_TRACE_ERROR0 ("Invalid i2c fix: unexpected end of patch");
        nfc_brcm_prm_spd_handle_download_complete (BRCM_PRM_ABORT_INVALID_PATCH_EVT);
    }
}
#endif /* NFC_I2C_PATCH_INCLUDED */

/*******************************************************************************
**
** Function         nfc_brcm_prm_spd_check_version
**
** Description      Check patchfile version with current downloaded version
**
** Returns          void
**
*******************************************************************************/
void nfc_brcm_prm_spd_check_version (void)
{
    UINT8 *p, *p_start, i;
    UINT32 patchfile_patch_present_mask;
    UINT16 patchfile_project_id;
    UINT16 patchfile_ver_major = 0;
    UINT16 patchfile_ver_minor;
    UINT16 patchfile_patchsize;

    UINT8  return_code = BRCM_PRM_COMPLETE_EVT;

    /* Initialize patchfile offset pointers */
    p = p_start = NULL;
    patchfile_patchsize = 0;

    /* Get patchfile version */
    if (nfc_brcm_cb.prm.cur_patch_len_remaining >= BRCM_PRM_NCD_PATCHFILE_HDR_LEN)
    {
        /* Parse patchfile header */
        p       = (UINT8 *) nfc_brcm_cb.prm.p_cur_patch_data;
        p_start = p;
        STREAM_TO_UINT16 (patchfile_project_id, p);
        STREAM_TO_UINT16 (patchfile_ver_major, p);
        STREAM_TO_UINT16 (patchfile_ver_minor, p);

        /* RFU */
        p++;

        /* Check how many patches are in the patch file */
        STREAM_TO_UINT8 (nfc_brcm_cb.prm.spd_patch_count, p);

        if (nfc_brcm_cb.prm.spd_patch_count > BRCM_PRM_MAX_PATCH_COUNT)
        {
            NCI_TRACE_ERROR2 ("Unsupported patchfile (number of patches (%i) exceeds maximum (%i)",
                               nfc_brcm_cb.prm.spd_patch_count, BRCM_PRM_MAX_PATCH_COUNT);
        }

        /* Mask of patches that are present in the patchfile */
        patchfile_patch_present_mask = 0;

        /* Get lengths for each patch */
        for (i = 0; i < nfc_brcm_cb.prm.spd_patch_count; i++)
        {
            /* Get power mode for this patch */
            STREAM_TO_UINT8 (nfc_brcm_cb.prm.spd_patch_desc[i].power_mode, p);

            /* Update mask of power-modes present in the patchfile */
            patchfile_patch_present_mask |= ((UINT32) 1 << nfc_brcm_cb.prm.spd_patch_desc[i].power_mode);

            /* Get length of patch */
            STREAM_TO_UINT16 (nfc_brcm_cb.prm.spd_patch_desc[i].len, p);

            /* Add total size of patches */
            patchfile_patchsize += nfc_brcm_cb.prm.spd_patch_desc[i].len;

            /* 5 byte RFU */
            p += 5;
        }

        /* Adjust offset to after the patch file header */
        nfc_brcm_cb.prm.cur_patch_offset += (UINT16) (p - p_start);              /* Bytes of patchfile transmitted/processed so far */
        nfc_brcm_cb.prm.cur_patch_len_remaining -= (UINT16) (p - p_start);       /* Adjust size of patchfile                        */


        NCI_TRACE_DEBUG6 ("Patchfile info: ProjID=0x%04x,  Ver=%i.%i, Num patches=%i, PatchMask=0x%08x, PatchSize=%i",
                           patchfile_project_id, patchfile_ver_major, patchfile_ver_minor,
                           nfc_brcm_cb.prm.spd_patch_count, patchfile_patch_present_mask, patchfile_patchsize);

        /*********************************************************************
        * Version check of patchfile against NVM
        *********************************************************************/

#if (!defined (BRCM_PRM_SKIP_VERSION_CHECK) || (BRCM_PRM_SKIP_VERSION_CHECK == FALSE))
        /* Download the patchfile if no patches in NVM */
        if ((nfc_brcm_cb.prm.spd_project_id == 0) || (nfc_brcm_cb.prm.spd_nvm_patch_mask == 0))
        {
            /* No patch in NVM, need to download all */
            nfc_brcm_cb.prm.spd_patch_needed_mask = patchfile_patch_present_mask;

            NCI_TRACE_DEBUG2 ("No previous patch detected. Downloading patch %i.%i",
                              patchfile_ver_major, patchfile_ver_minor);
        }
        /* Skip download if project ID of patchfile does not match NVM */
        else if (nfc_brcm_cb.prm.spd_project_id != patchfile_project_id)
        {
            /* Project IDs mismatch */
            NCI_TRACE_DEBUG2 ("Patch download skipped: Mismatched Project ID (NVM ProjId: 0x%04x, Patchfile ProjId: 0x%04x)",
                              nfc_brcm_cb.prm.spd_project_id, patchfile_project_id);

            return_code = BRCM_PRM_ABORT_INVALID_PATCH_EVT;
        }
        /* Skip download if version of patchfile older or equal to version in NVM */
        /* unless NVM is corrupted (then don't skip download if patchfile has the same major ver)*/
        else if (  (nfc_brcm_cb.prm.spd_ver_major > patchfile_ver_major)
                 ||(  (nfc_brcm_cb.prm.spd_ver_major == patchfile_ver_major) && (nfc_brcm_cb.prm.spd_ver_minor == patchfile_ver_minor)
                    && !((patchfile_patch_present_mask & ( 1 << BRCM_PRM_SPD_POWER_MODE_LPM)) && (nfc_brcm_cb.prm.spd_lpm_patch_size == 0))  /* Do not skip download: patchfile has LPM, but NVM does not */
                    && !((patchfile_patch_present_mask & ( 1 << BRCM_PRM_SPD_POWER_MODE_FPM)) && (nfc_brcm_cb.prm.spd_fpm_patch_size == 0))  /* Do not skip download: patchfile has FPM, but NVM does not */
                    && !(nfc_brcm_cb.prm.flags & (BRCM_PRM_FLAGS_NVM_FPM_CORRUPTED |BRCM_PRM_FLAGS_NVM_LPM_CORRUPTED))  )  )
        {
            /* NVM version is newer than patchfile */
            NCI_TRACE_DEBUG2 ("Patch download skipped. NVM patch (version %i.%i) is newer than the patchfile ",
                              nfc_brcm_cb.prm.spd_ver_major, nfc_brcm_cb.prm.spd_ver_minor);

            return_code = BRCM_PRM_COMPLETE_EVT;
        }
        /* Remaining cases: patchfile major version is newer than NVM; or major version is the same with different minor version */
        /* Download all patches in the patchfile */
        else
        {
            nfc_brcm_cb.prm.spd_patch_needed_mask = patchfile_patch_present_mask;

            NCI_TRACE_DEBUG4 ("Downloading patch version: %i.%i (previous version in NVM: %i.%i)...",
                              patchfile_ver_major, patchfile_ver_minor,
                              nfc_brcm_cb.prm.spd_ver_major, nfc_brcm_cb.prm.spd_ver_minor);
        }
#else   /* BRCM_PRM_SKIP_VERSION_CHECK */
        nfc_brcm_cb.prm.spd_patch_needed_mask = patchfile_patch_present_mask;
#endif
    }
    else
    {
        /* Invalid patch file header */
        NCI_TRACE_ERROR0 ("Invalid patch file header.");

        return_code = BRCM_PRM_ABORT_INVALID_PATCH_EVT;
    }

    /* Validate sizes */
    if ((nfc_brcm_cb.prm.spd_patch_needed_mask) && (patchfile_patchsize > nfc_brcm_cb.prm.spd_patch_max_size) && (nfc_brcm_cb.prm.spd_patch_max_size != 0))
    {
        /* Invalid patch file header */
        NCI_TRACE_ERROR2 ("Patchfile patch sizes (%i) is greater than NVM patch memory size (%i)",
                          patchfile_patchsize, nfc_brcm_cb.prm.spd_patch_max_size);
        nfc_brcm_cb.prm.spd_patch_needed_mask = 0;
        return_code = BRCM_PRM_ABORT_INVALID_PATCH_EVT;
    }

    /* If we need to download anything, get the first patch to download */
    if (nfc_brcm_cb.prm.spd_patch_needed_mask)
    {
#if (defined (NFC_I2C_PATCH_INCLUDED) && (NFC_I2C_PATCH_INCLUDED == TRUE))
        /* Check if I2C patch is needed: if                                     */
        /*      - I2C patch file was provided using PRM_SetI2cPatch, and        */
        /*      -   current patch in NVM has ProjectID=0, or                    */
        /*          FPM is not present or corrupted, or                         */
        /*          or patchfile is major-ver 76+                               */
        if (  (nfc_brcm_cb.prm_i2c.p_patch)
            &&(  (nfc_brcm_cb.prm.spd_project_id == 0)
               ||(nfc_brcm_cb.prm.spd_fpm_patch_size == 0)
               ||(nfc_brcm_cb.prm.flags & BRCM_PRM_FLAGS_NVM_FPM_CORRUPTED)
               ||(patchfile_ver_major >= 76)))
        {
            BT_TRACE_0 (TRACE_LAYER_HCI, TRACE_TYPE_DEBUG, "I2C patch fix required.");
            nfc_brcm_cb.prm.flags |= BRCM_PRM_FLAGS_I2C_FIX_REQUIRED;

            /* Download i2c fix first */
            nfc_brcm_prm_spd_download_i2c_fix ();
            return;
        }
#endif  /* NFC_I2C_PATCH_INCLUDED */

        /* Download first segment */
        nfc_brcm_cb.prm.state = BRCM_PRM_ST_SPD_GET_PATCH_HEADER;
        if (!(nfc_brcm_cb.prm.flags & BRCM_PRM_FLAGS_USE_PATCHRAM_BUF))
        {
            /* Notify adaptation layer to call PRM_DownloadContinue with the next patch segment */
            (nfc_brcm_cb.prm.p_cback) (BRCM_PRM_SPD_GET_NEXT_PATCH);
        }
        else
        {
            nfc_brcm_prm_spd_handle_next_patch_start ();
        }
    }
    else
    {
        /* Download complete */
        nfc_brcm_prm_spd_handle_download_complete (return_code);
    }
}

#if (BT_TRACE_VERBOSE == TRUE)
/*******************************************************************************
**
** Function         nfc_brcm_prm_spd_status_str
**
** Description      Return status string for a given spd status code
**
** Returns          Status string
**
*******************************************************************************/
UINT8 *nfc_brcm_prm_spd_status_str (UINT8 spd_status_code)
{
    char *p_str;

    switch (spd_status_code)
    {
    case NCI_STATUS_SPD_ERROR_DEST:
        p_str = "SPD_ERROR_DEST";
        break;

    case NCI_STATUS_SPD_ERROR_PROJECTID:
        p_str = "SPD_ERROR_PROJECTID";
        break;

    case NCI_STATUS_SPD_ERROR_CHIPVER:
        p_str = "SPD_ERROR_CHIPVER";
        break;

    case NCI_STATUS_SPD_ERROR_MAJORVER:
        p_str = "SPD_ERROR_MAJORVER";
        break;

    case NCI_STATUS_SPD_ERROR_INVALID_PARAM:
        p_str = "SPD_ERROR_INVALID_PARAM";
        break;

    case NCI_STATUS_SPD_ERROR_INVALID_SIG:
        p_str = "SPD_ERROR_INVALID_SIG";
        break;

    case NCI_STATUS_SPD_ERROR_NVM_CORRUPTED:
        p_str = "SPD_ERROR_NVM_CORRUPTED";
        break;

    case NCI_STATUS_SPD_ERROR_PWR_MODE:
        p_str = "SPD_ERROR_PWR_MODE";
        break;

    case NCI_STATUS_SPD_ERROR_MSG_LEN:
        p_str = "SPD_ERROR_MSG_LEN";
        break;

    case NCI_STATUS_SPD_ERROR_PATCHSIZE:
        p_str = "SPD_ERROR_PATCHSIZE";
        break;

    default:
        p_str = "Unspecified Error";
        break;

    }

    return ((UINT8*) p_str);
}
#endif  /* (BT_TRACE_VERBOSE == TRUE) */

/*******************************************************************************
**
** Function         nfc_brcm_prm_nci_command_complete_cback
**
** Description      Callback for NCI vendor specific command complete
**                  (for secure patch download)
**
** Returns          void
**
*******************************************************************************/
void nfc_brcm_prm_nci_command_complete_cback (tNFC_VS_EVT event, UINT16 data_len, UINT8 *p_data)
{
    UINT8 status, u8;
    UINT8 *p;
    UINT32 post_signature_delay;

    NFC_BRCM_PRM_STATE ("nfc_brcm_prm_nci_command_complete_cback");

    /* Stop the command-timeout timer */
    ncit_stop_quick_timer (&nci_brcm_cb.nci_wait_rsp_timer);

    /* Skip over NCI header */
    p = p_data + NCI_MSG_HDR_SIZE;

    /* Handle GET_PATCH_VERSION Rsp */
    if (event == NFC_VS_GET_PATCH_VERSION_EVT)
    {
        /* Get project id */
        STREAM_TO_UINT16 (nfc_brcm_cb.prm.spd_project_id, p);

        /* RFU */
        p++;

        /* Get chip version string */
        STREAM_TO_UINT8 (u8, p);
        p += BRCM_PRM_NCD_PATCH_VERSION_LEN;

        /* Get major/minor version */
        STREAM_TO_UINT16 (nfc_brcm_cb.prm.spd_ver_major, p);
        STREAM_TO_UINT16 (nfc_brcm_cb.prm.spd_ver_minor, p);
        STREAM_TO_UINT16 (nfc_brcm_cb.prm.spd_nvm_max_size, p);
        STREAM_TO_UINT16 (nfc_brcm_cb.prm.spd_patch_max_size, p);
        STREAM_TO_UINT16 (nfc_brcm_cb.prm.spd_lpm_patch_size, p);
        STREAM_TO_UINT16 (nfc_brcm_cb.prm.spd_fpm_patch_size, p);

        /* LPMPatchCodeHasBadCRC (if not bad crc, then indicate LPM patch is present in nvm) */
        STREAM_TO_UINT8 (u8, p);
        if (!u8)
        {
            nfc_brcm_cb.prm.spd_nvm_patch_mask |= (1 << BRCM_PRM_SPD_POWER_MODE_LPM);
        }
        else
        {
            /* LPM patch in NVM fails CRC check */
            nfc_brcm_cb.prm.flags |= BRCM_PRM_FLAGS_NVM_LPM_CORRUPTED;
        }


        /* FPMPatchCodeHasBadCRC (if not bad crc, then indicate LPM patch is present in nvm) */
        STREAM_TO_UINT8 (u8, p);
        if (!u8)
        {
            nfc_brcm_cb.prm.spd_nvm_patch_mask |= (1 << BRCM_PRM_SPD_POWER_MODE_FPM);
        }
        else
        {
            /* FPM patch in NVM fails CRC check */
            nfc_brcm_cb.prm.flags |= BRCM_PRM_FLAGS_NVM_FPM_CORRUPTED;
        }

        /* Check if downloading patch to RAM only (no NVM) */
        STREAM_TO_UINT8 (u8, p);
        if (!u8)
            nfc_brcm_cb.prm.flags |= BRCM_PRM_FLAGS_NO_NVM;

        /* Get patchfile version number */
        nfc_brcm_cb.prm.state = BRCM_PRM_ST_SPD_COMPARE_VERSION;

        if (nfc_brcm_cb.prm.flags & BRCM_PRM_FLAGS_USE_PATCHRAM_BUF)
        {
            /* If patch is in a buffer, get patch version from buffer */
            nfc_brcm_prm_spd_check_version ();
        }
        else
        {
            /* Notify adaptation layer to send patch version (via PRM_DownloadContinue) */
            (nfc_brcm_cb.prm.p_cback) (BRCM_PRM_SPD_GET_PATCHFILE_HDR_EVT);
        }

    }
    /* Handle SECURE_PATCH_DOWNLOAD Rsp */
    else if (event == NFC_VS_SEC_PATCH_DOWNLOAD_EVT)
    {
        /* Status and error code */
        STREAM_TO_UINT8 (status, p);
        STREAM_TO_UINT8 (u8, p);

        if (status != NCI_STATUS_OK)
        {
#if (BT_TRACE_VERBOSE == TRUE)
            NCI_TRACE_ERROR2 ("Patch download failed, reason code=0x%X (%s)", status, nfc_brcm_prm_spd_status_str (status));
#else
            NCI_TRACE_ERROR1 ("Patch download failed, reason code=0x%X", status);
#endif

            /* Notify application */
            nfc_brcm_prm_spd_handle_download_complete (BRCM_PRM_ABORT_INVALID_PATCH_EVT);
            return;
        }

        /* If last segment (SIGNATURE) sent */
        if (nfc_brcm_cb.prm.flags & BRCM_PRM_FLAGS_SIGNATURE_SENT)
        {
            /* Wait for authentication complate (SECURE_PATCH_DOWNLOAD NTF) */
            nfc_brcm_cb.prm.state = BRCM_PRM_ST_SPD_AUTHENTICATING;
            ncit_start_quick_timer (&nci_brcm_cb.nci_wait_rsp_timer, 0x00,
                                   (BRCM_PRM_SPD_TOUT * QUICK_TIMER_TICKS_PER_SEC) / 1000);
            return;
        }
        /* Download next segment */
        else if (nfc_brcm_cb.prm.flags & BRCM_PRM_FLAGS_USE_PATCHRAM_BUF)
        {
            /* If patch is in a buffer, get next patch from buffer */
            nfc_brcm_prm_spd_send_next_segment ();
        }
        else
        {
            /* Notify adaptation layer to get next patch segment (via PRM_DownloadContinue) */
            (nfc_brcm_cb.prm.p_cback) (BRCM_PRM_CONTINUE_EVT);
        }
    }
    /* Handle SECURE_PATCH_DOWNLOAD NTF */
    else if (event == NFC_VS_SEC_PATCH_AUTH_EVT)
    {
        NCI_TRACE_DEBUG1 ("prm flags:0x%x.", nfc_brcm_cb.prm.flags);
        /* Status and error code */
        STREAM_TO_UINT8 (status, p);
        STREAM_TO_UINT8 (u8, p);

        /* Sanity check - should only get this NTF while in AUTHENTICATING stage */
        if (nfc_brcm_cb.prm.state == BRCM_PRM_ST_SPD_AUTHENTICATING)
        {
            if (status != NCI_STATUS_OK)
            {
                NCI_TRACE_ERROR0 ("Patch authentication failed");
                nfc_brcm_prm_spd_handle_download_complete (BRCM_PRM_ABORT_BAD_SIGNATURE_EVT);
                return;
            }

#if (defined (NFC_I2C_PATCH_INCLUDED) && (NFC_I2C_PATCH_INCLUDED == TRUE))
            if (nfc_brcm_cb.prm.flags & BRCM_PRM_FLAGS_I2C_FIX_REQUIRED)
            {
                NCI_TRACE_DEBUG1 ("PreI2C patch downloaded...waiting %i ms for NFCC to reboot.", BRCM_PRM_POST_I2C_FIX_DELAY);

                /* Restore pointers to patchfile */
                nfc_brcm_cb.prm.flags &= ~BRCM_PRM_FLAGS_I2C_FIX_REQUIRED;
                nfc_brcm_cb.prm.p_cur_patch_data = nfc_brcm_cb.prm.p_spd_patch;
                nfc_brcm_cb.prm.cur_patch_offset = nfc_brcm_cb.prm.spd_patch_offset;
                nfc_brcm_cb.prm.cur_patch_len_remaining = nfc_brcm_cb.prm.spd_patch_len_remaining;

                /* Resume normal patch download */
                nfc_brcm_cb.prm.state = BRCM_PRM_ST_SPD_GET_PATCH_HEADER;
                nfc_brcm_cb.prm.flags &= ~BRCM_PRM_FLAGS_SIGNATURE_SENT;

                /* Post PreI2C delay */
                ncit_start_quick_timer (&nci_brcm_cb.nci_wait_rsp_timer, 0x00, (BRCM_PRM_POST_I2C_FIX_DELAY * QUICK_TIMER_TICKS_PER_SEC) / 1000);

                return;
            }
#endif  /* NFC_I2C_PATCH_INCLUDED */


            /* Wait for NFCC to save the patch to NVM */
            if (nfc_brcm_cb.prm.flags & BRCM_PRM_FLAGS_SUPPORT_RESET_NTF)
            {
                /* 20791B4 or newer - wait for RESET_NTF */
                post_signature_delay = BRCM_PRM_RESET_NTF_DELAY;
                NCI_TRACE_DEBUG1 ("Patch downloaded and authenticated. Waiting %i ms for RESET NTF...", post_signature_delay);

            }
            else if (nfc_brcm_cb.prm.flags & BRCM_PRM_FLAGS_NO_NVM)
            {
                /* No NVM. Wait for NFCC to restart */
                post_signature_delay = BRCM_PRM_END_DELAY;
                NCI_TRACE_DEBUG1 ("Patch downloaded and authenticated. Waiting %i ms for NFCC to restart...", post_signature_delay);
            }
            else
            {
                /* Wait for NFCC to save the patch to NVM (need about 1 ms per byte) */
                post_signature_delay = nfc_brcm_cb.prm.spd_patch_desc[nfc_brcm_cb.prm.spd_cur_patch_idx].len;
                NCI_TRACE_DEBUG1 ("Patch downloaded and authenticated. Waiting %i ms for NVM update to complete...", post_signature_delay);
            }

            nfc_brcm_cb.prm.state = BRCM_PRM_ST_SPD_AUTH_DONE;

            ncit_start_quick_timer (&nci_brcm_cb.nci_wait_rsp_timer, 0x00,
                                   (post_signature_delay * QUICK_TIMER_TICKS_PER_SEC) / 1000);
        }
        else
        {
            NCI_TRACE_ERROR0 ("Got unexpected SECURE_PATCH_DOWNLOAD NTF");
            nfc_brcm_prm_spd_handle_download_complete (BRCM_PRM_ABORT_EVT);
        }
    }
    else
    {
        /* Invalid response from NFCC during patch download */
        NCI_TRACE_ERROR1 ("Invalid response from NFCC during patch download (opcode=0x%02X)", event);
        nfc_brcm_prm_spd_handle_download_complete (BRCM_PRM_ABORT_INVALID_PATCH_EVT);
    }

    NFC_BRCM_PRM_STATE ("prm_nci_command_complete_cback");
}

/*******************************************************************************
**
** Function         nfc_brcm_prm_nfcc_ready_to_continue
**
** Description      Continue to download patch or notify application completition
**
** Returns          void
**
*******************************************************************************/
void nfc_brcm_prm_nfcc_ready_to_continue (void)
{
    /* Clear the bit for the patch we just downloaded */
    nfc_brcm_cb.prm.spd_patch_needed_mask &= ~ ((UINT32) 1 << nfc_brcm_cb.prm.spd_patch_desc[nfc_brcm_cb.prm.spd_cur_patch_idx].power_mode);

    /* Check if another patch to download */
    nfc_brcm_cb.prm.spd_cur_patch_idx++;
    if ((nfc_brcm_cb.prm.spd_patch_needed_mask) && (nfc_brcm_cb.prm.spd_cur_patch_idx < nfc_brcm_cb.prm.spd_patch_count))
    {
        nfc_brcm_cb.prm.state = BRCM_PRM_ST_SPD_GET_PATCH_HEADER;
        nfc_brcm_cb.prm.flags &= ~BRCM_PRM_FLAGS_SIGNATURE_SENT;

        if (nfc_brcm_cb.prm.flags & BRCM_PRM_FLAGS_USE_PATCHRAM_BUF)
        {
            /* If patch is in a buffer, get next patch from buffer */
            nfc_brcm_prm_spd_handle_next_patch_start ();
        }
        else
        {
            /* Notify adaptation layer to get next patch header (via PRM_DownloadContinue) */
            (nfc_brcm_cb.prm.p_cback) (BRCM_PRM_SPD_GET_NEXT_PATCH);
        }

    }
    else
    {
        /* Done downloading */
        NCI_TRACE_DEBUG0 ("Patch downloaded and authenticated.");
        nfc_brcm_prm_spd_handle_download_complete (BRCM_PRM_COMPLETE_EVT);
    }
}

/*******************************************************************************
**
** Function         nfc_brcm_prm_spd_reset_ntf
**
** Description      Received RESET NTF from NFCC, indicating it has completed
**                  reset after patch download.
**
** Returns          void
**
*******************************************************************************/
void nfc_brcm_prm_spd_reset_ntf (UINT8 reset_reason, UINT8 reset_type)
{
    /* Check if we were expecting a RESET NTF */
    if (nfc_brcm_cb.prm.state == BRCM_PRM_ST_SPD_AUTH_DONE)
    {
        NCI_TRACE_DEBUG2 ("Received RESET NTF after patch download (reset_reason=%i, reset_type=%i)", reset_reason, reset_type);

        /* Stop waiting for RESET NTF */
        ncit_stop_quick_timer (&nci_brcm_cb.nci_wait_rsp_timer);

        {
        /* Continue with patch download */
        nfc_brcm_prm_nfcc_ready_to_continue ();
    }
    }
    else
    {
        NCI_TRACE_ERROR2 ("Received unexpected RESET NTF (reset_reason=%i, reset_type=%i)", reset_reason, reset_type);
    }
}

/*******************************************************************************
**
** Function:    nfc_post_final_baud_update
**
** Description: Called after baud rate udate
**
** Returns:     Nothing
**
*******************************************************************************/
void nfc_brcm_prm_post_baud_update (tNFC_STATUS status)
{
    NFC_BRCM_PRM_STATE ("nfc_brcm_prm_post_baud_update");

    if (nfc_brcm_cb.prm.state == BRCM_PRM_ST_SPD_AUTH_DONE)
    {
        /* Proceed with next step of patch download sequence */
        nfc_brcm_prm_nfcc_ready_to_continue ();
    }
}

/*******************************************************************************
**
** Function         nfc_brcm_prm_process_timeout
**
** Description      Process timer expireation for patch download
**
** Returns          void
**
*******************************************************************************/
static void nfc_brcm_prm_process_timeout (void *p_tle)
{
    NFC_BRCM_PRM_STATE ("nfc_brcm_prm_process_timeout");

    if (nfc_brcm_cb.prm.state == BRCM_PRM_ST_IDLE)
    {
        nfc_brcm_cb.prm.state = BRCM_PRM_ST_SPD_GET_VERSION;

        /* Get currently downloaded patch version */
        nci_brcm_send_nci_cmd (brcm_prm_get_patch_version_cmd, NCI_MSG_HDR_SIZE, nfc_brcm_prm_nci_command_complete_cback);
    }
    else if (nfc_brcm_cb.prm.state == BRCM_PRM_ST_SPD_AUTH_DONE)
    {
        if (nfc_brcm_cb.prm.flags & BRCM_PRM_FLAGS_SUPPORT_RESET_NTF)
        {
            /* Timeout waiting for RESET NTF after signature sent */
            NCI_TRACE_ERROR0 ("Timeout waiting for RESET NTF after patch download");
            nfc_brcm_prm_spd_handle_download_complete (BRCM_PRM_ABORT_EVT);
        }
        else
        {
            nfc_brcm_prm_nfcc_ready_to_continue ();
        }
    }
    else if (nfc_brcm_cb.prm.state == BRCM_PRM_ST_SPD_GET_PATCH_HEADER)
    {
        NCI_TRACE_DEBUG0 ("Delay after PreI2C patch download...proceeding to download firmware patch");
        nfc_brcm_prm_spd_handle_next_patch_start ();
    }
    else
    {
        NCI_TRACE_ERROR1 ("Patch download: command timeout (state=%i)", nfc_brcm_cb.prm.state);

        nfc_brcm_prm_spd_handle_download_complete (BRCM_PRM_ABORT_EVT);
    }

    NFC_BRCM_PRM_STATE ("nfc_brcm_prm_process_timeout");
}


/*******************************************************************************
**
** Function         PRM_DownloadStart
**
** Description      Initiate patch download
**
** Input Params
**                  format_type     patch format type
**                                  (BRCM_PRM_FORMAT_BIN, BRCM_PRM_FORMAT_HCD, or
**                                   BRCM_PRM_FORMAT_NCD)
**
**                  dest_address    destination adderess (needed for BIN format only)
**
**                  p_patchram_buf  pointer to patchram buffer. If NULL,
**                                  then app must call PRM_DownloadContinue when
**                                  PRM_CONTINUE_EVT is received, to send the next
**                                  segment of patchram
**
**                  patchram_len    size of p_patchram_buf (if non-NULL)
**
**                  p_cback         callback for download status
**
**
** Returns          TRUE if successful, otherwise FALSE
**
**
*******************************************************************************/
BOOLEAN PRM_DownloadStart (tBRCM_PRM_FORMAT format_type,
                           UINT32           dest_address,
                           UINT8            *p_patchram_buf,
                           UINT32           patchram_len,
                           tBRCM_PRM_CBACK  *p_cback)
{
    NCI_TRACE_API0 ("PRM_DownloadStart ()");

    memset (&nfc_brcm_cb.prm, 0, sizeof (tBRCM_PRM_CB));

    if (p_patchram_buf)
    {
        nfc_brcm_cb.prm.p_cur_patch_data = p_patchram_buf;
        nfc_brcm_cb.prm.cur_patch_offset = 0;
        nfc_brcm_cb.prm.cur_patch_len_remaining = (UINT16) patchram_len;
        nfc_brcm_cb.prm.flags |= BRCM_PRM_FLAGS_USE_PATCHRAM_BUF;

        if (patchram_len == 0)
            return FALSE;
    }

    nfc_brcm_cb.prm.p_cback  = p_cback;
    nfc_brcm_cb.prm.dest_ram = dest_address;
    nfc_brcm_cb.prm.format   = format_type;

    nci_brcm_cb.nci_wait_rsp_timer.p_cback = nfc_brcm_prm_process_timeout;

    if (format_type == BRCM_PRM_FORMAT_NCD)
    {
        /* Store patch buffer pointer and length */
        nfc_brcm_cb.prm.p_spd_patch             = p_patchram_buf;
        nfc_brcm_cb.prm.spd_patch_len_remaining = (UINT16)patchram_len;
        nfc_brcm_cb.prm.spd_patch_offset        = 0;

        /* Need delay for controller to finish resetting */
        ncit_start_quick_timer (&nci_brcm_cb.nci_wait_rsp_timer, 0x00,
                               (BRCM_PRM_SPD_PRE_DOWNLOAD_DELAY * QUICK_TIMER_TICKS_PER_SEC) / 1000);
    }
    else
    {
        NCI_TRACE_ERROR0 ("Unexpected patch format.");
        return FALSE;
    }

    return TRUE;
}

/*******************************************************************************
**
** Function         PRM_DownloadContinue
**
** Description      Send next segment of patchram to controller. Called when
**                  PRM_CONTINUE_EVT is received.
**
**                  Only needed if PRM_DownloadStart was called with
**                  p_patchram_buf=NULL
**
** Input Params     p_patch_data    pointer to patch data
**                  patch_data_len  patch data len
**
** Returns          TRUE if successful, otherwise FALSE
**
*******************************************************************************/
BOOLEAN PRM_DownloadContinue (UINT8 *p_patch_data,
                              UINT16 patch_data_len)
{
    NCI_TRACE_API2 ("PRM_DownloadContinue ():state = %d, patch_data_len=%d",
                     nfc_brcm_cb.prm.state, patch_data_len);

    /* Check if we are in a valid state for this API */
    if (  (nfc_brcm_cb.prm.state != BRCM_PRM_ST_SPD_COMPARE_VERSION)
        &&(nfc_brcm_cb.prm.state != BRCM_PRM_ST_SPD_GET_PATCH_HEADER)
        &&(nfc_brcm_cb.prm.state != BRCM_PRM_ST_SPD_DOWNLOADING)  )
        return FALSE;

    if (patch_data_len == 0)
        return FALSE;

    nfc_brcm_cb.prm.cur_patch_offset = 0;
    nfc_brcm_cb.prm.p_cur_patch_data = p_patch_data;
    nfc_brcm_cb.prm.cur_patch_len_remaining = patch_data_len;

    /* Call appropriate handler */
    if (nfc_brcm_cb.prm.state == BRCM_PRM_ST_SPD_COMPARE_VERSION)
    {
        nfc_brcm_prm_spd_check_version ();
    }
    else if (nfc_brcm_cb.prm.state == BRCM_PRM_ST_SPD_GET_PATCH_HEADER)
    {
        nfc_brcm_prm_spd_handle_next_patch_start ();
    }
    else if (nfc_brcm_cb.prm.state == BRCM_PRM_ST_SPD_DOWNLOADING)
    {
        nfc_brcm_prm_spd_send_next_segment ();
    }
    else
    {
        NCI_TRACE_ERROR1 ("Unexpected patch state:%d.", nfc_brcm_cb.prm.state);
    }

    return TRUE;
}

/*******************************************************************************
**
** Function         PRM_SetI2cPatch
**
** Description      Specify patchfile for BCM20791B3 I2C fix. This fix
**                  must be downloaded prior to initial patch download for I2C
**                  transport
**
** Input Params     p_i2c_patchfile_buf: pointer to patch for i2c fix
**                  i2c_patchfile_len: length of patch
**
**
** Returns          Nothing
**
**
*******************************************************************************/
void PRM_SetI2cPatch (UINT8 *p_i2c_patchfile_buf, UINT16 i2c_patchfile_len)
{
#if (defined (NFC_I2C_PATCH_INCLUDED) && (NFC_I2C_PATCH_INCLUDED == TRUE))
    NCI_TRACE_API0 ("PRM_SetI2cPatch ()");

    nfc_brcm_cb.prm_i2c.p_patch = p_i2c_patchfile_buf;
    nfc_brcm_cb.prm_i2c.len = i2c_patchfile_len;
#endif  /* NFC_I2C_PATCH_INCLUDED */
}

/*******************************************************************************
**
** Function         PRM_SetSpdNciCmdPayloadSize
**
** Description      Set Host-to-NFCC NCI message size for secure patch download
**
**                  This API must be called before calling PRM_DownloadStart.
**                  If the API is not called, then PRM will use the default
**                  message size.
**
**                  Typically, this API is only called for platforms that have
**                  message-size limitations in the transport/driver.
**
**                  Valid message size range: BRCM_PRM_MIN_NCI_CMD_PAYLOAD_SIZE to 255.
**
** Returns          NFC_STATUS_OK if successful
**                  NFC_STATUS_INVALID_PARAM otherwise
**
**
*******************************************************************************/
tNFC_STATUS PRM_SetSpdNciCmdPayloadSize (UINT8 max_payload_size)
{
    /* Validate: minimum size is BRCM_PRM_MIN_NCI_CMD_PAYLOAD_SIZE */
    if (max_payload_size < BRCM_PRM_MIN_NCI_CMD_PAYLOAD_SIZE)
    {
        BT_TRACE_2 (TRACE_LAYER_HCI, TRACE_TYPE_ERROR, "PRM_SetSpdNciCmdPayloadSize: invalid size (%i). Must be between %i and 255", max_payload_size, BRCM_PRM_MIN_NCI_CMD_PAYLOAD_SIZE);
        return (NFC_STATUS_INVALID_PARAM);
    }
    else
    {
        BT_TRACE_1 (TRACE_LAYER_HCI, TRACE_TYPE_API, "PRM_SetSpdNciCmdPayloadSize: new message size during download: %i", max_payload_size);
        nfc_cb.nci_ctrl_size = max_payload_size;
        return (NFC_STATUS_OK);
    }
}

#endif /* (NFC_BRCM_VS_INCLUDED == TRUE) */
