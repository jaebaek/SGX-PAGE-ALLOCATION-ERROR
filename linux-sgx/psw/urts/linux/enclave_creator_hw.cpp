/*
 * Copyright (C) 2011-2016 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include "enclave_creator_hw.h"
#include "se_trace.h"
#include "se_page_attr.h"
#include "isgx_user.h"
#include "sig_handler.h"
#include "se_error_internal.h"
#include "se_memcpy.h"
#include "se_atomic.h"
#include "se_detect.h"
#include "cpuid.h"
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdlib.h>


EnclaveCreator* g_enclave_creator = new EnclaveCreatorHW();
static uint32_t g_eid_low = 0x1;
static uint32_t g_eid_high = 0x0;

EnclaveCreatorHW::EnclaveCreatorHW():
    m_hdevice(-1),
    m_sig_registered(false)
{
    se_mutex_init(&m_sig_mutex);
}

EnclaveCreatorHW::~EnclaveCreatorHW()
{
    close_se_device();
}

int EnclaveCreatorHW::error_driver2urts(int driver_error)
{
    int ret = SGX_ERROR_UNEXPECTED;

    switch(driver_error)
    {
    case ISGX_ERROR:
        if(ENOMEM == errno)
            ret = SGX_ERROR_OUT_OF_MEMORY;
        else
            ret = SGX_ERROR_NO_DEVICE;
        break;
    case ISGX_INVALID_ATTRIBUTE:
        ret = SGX_ERROR_INVALID_ATTRIBUTE;
        break;
    case ISGX_INVALID_MEASUREMENT:
        ret = SE_ERROR_INVALID_MEASUREMENT;
        break;
    case ISGX_INVALID_SIG_STRUCT:
    case ISGX_INVALID_SIGNATIRE:
        ret = SGX_ERROR_INVALID_SIGNATURE;
        break;
    case ISGX_INVALID_LAUNCH_TOKEN:
        ret = SE_ERROR_INVALID_LAUNCH_TOKEN;
        break;
    case ISGX_INVALID_CPUSVN:
        ret = SGX_ERROR_INVALID_CPUSVN;
        break;
    case ISGX_INVALID_ISVSVN:
        ret = SGX_ERROR_INVALID_ISVSVN;
        break;
    case ISGX_UNMASKED_EVENT:
        ret = SGX_ERROR_DEVICE_BUSY;
        break;
    case (int)ISGX_POWER_LOST_ENCLAVE: // [-Wc++11-narrowing]
        ret = SGX_ERROR_ENCLAVE_LOST;
        break;
    default:
        SE_TRACE(SE_TRACE_WARNING, "unexpected error %#x from driver, should be uRTS/driver bug\n", ret);
        ret = SGX_ERROR_UNEXPECTED;
        break;
    }

    return ret;
}

int EnclaveCreatorHW::create_enclave(secs_t *secs, sgx_enclave_id_t *enclave_id, void **start_addr, bool ae)
{
    assert(secs != NULL && enclave_id != NULL && start_addr != NULL);
    UNUSED(ae);

    if (false == open_se_device())
        return SGX_ERROR_NO_DEVICE;

    SE_TRACE(SE_TRACE_DEBUG, "\n secs.attibutes.flags = %llx, secs.attributes.xfrm = %llx \n"
             , secs->attributes.flags, secs->attributes.xfrm);

    struct isgx_create_param param = { secs, 0 };
    int ret = ioctl(m_hdevice, ISGX_IOCTL_ENCLAVE_CREATE, &param);
    if(ret) {
        SE_TRACE(SE_TRACE_WARNING, "\nISGX_IOCTL_ENCLAVE_CREATE fails: errno = %x\n", errno);
        return error_driver2urts(ret);
    }
    uint32_t tmp = se_atomic_inc(&g_eid_low);
    //32bit overflow
    if(0 == tmp)
        g_eid_high++;
    *enclave_id = ((uint64_t)g_eid_high << 32) | g_eid_low;
    *start_addr = secs->base = (void *)param.addr;

    return SGX_SUCCESS;
}

int EnclaveCreatorHW::add_enclave_page(sgx_enclave_id_t enclave_id, void *src, uint64_t rva, const sec_info_t &sinfo, uint32_t attr)
{
    assert((rva & ((1<<SE_PAGE_SHIFT)-1)) == 0);
    void* source = src;
    uint8_t color_page[SE_PAGE_SIZE] = { 0 };
    if(NULL == source)
    {
        memset(color_page, 0, SE_PAGE_SIZE);
        source = reinterpret_cast<void*>(&color_page);
    }

    int ret = 0;
    struct isgx_add_param addp = { 0, 0, 0, 0 };

    addp.addr = (unsigned long)enclave_id + (unsigned long)rva;
    addp.user_addr = reinterpret_cast<unsigned long>(source);
    addp.secinfo = (void *)const_cast<sec_info_t *>(&sinfo);
    if(!((1<<DoEEXTEND) & attr))
        addp.flags |= ISGX_ADD_SKIP_EEXTEND;
    ret = ioctl(m_hdevice, ISGX_IOCTL_ENCLAVE_ADD_PAGE, &addp);
    if(ret) {
        SE_TRACE(SE_TRACE_WARNING, "\nAdd Page - %p to %p... FAIL\n", source, rva);
        return error_driver2urts(ret);
    }
   
    return SGX_SUCCESS;
}

int EnclaveCreatorHW::try_init_enclave(sgx_enclave_id_t enclave_id, enclave_css_t *enclave_css, token_t *launch)
{
    int ret = 0;
    struct isgx_init_param initp = { 0, NULL, NULL };
    initp.addr = (unsigned long)enclave_id;
    initp.sigstruct = reinterpret_cast<char*>(enclave_css);
    //launch should NOT be NULL, because it has been checked in urts_com.h::_create_enclave(...)
    assert(launch != NULL);

    initp.einittoken = reinterpret_cast<void *>(launch);
    ret = ioctl(m_hdevice, ISGX_IOCTL_ENCLAVE_INIT, &initp);
    if (ret) {
        SE_TRACE(SE_TRACE_WARNING, "\nISGX_IOCTL_ENCLAVE_INIT fails error = %x\n", ret);
        return error_driver2urts(ret);
    }

    //register signal handler
    se_mutex_lock(&m_sig_mutex);
    if(false == m_sig_registered)
    {
        reg_sig_handler();
        m_sig_registered = true;
    }
    se_mutex_unlock(&m_sig_mutex);

    return SGX_SUCCESS;
}

int EnclaveCreatorHW::destroy_enclave(sgx_enclave_id_t enclave_id)
{
    int ret = 0;

    isgx_destroy_param param = { (unsigned long)enclave_id };
    ret = ioctl(m_hdevice, ISGX_IOCTL_ENCLAVE_DESTROY, &param);

    if (-1 == ret) {
        SE_TRACE(SE_TRACE_WARNING, "destroy SGX enclave failed, error = %d\n", errno);
        ret = SGX_ERROR_UNEXPECTED;
    }
    else
    {
        ret = SGX_SUCCESS;
    }

    return ret;
}

bool EnclaveCreatorHW::get_plat_cap(sgx_misc_attribute_t *misc_attr)
{
    return get_plat_cap_by_cpuid(misc_attr);
}



bool EnclaveCreatorHW::open_se_device()
{
    LockGuard lock(&m_dev_mutex);
    int fd = -1;

    if(-1 != m_hdevice)
    {
        return true;
    }

    fd = open("/dev/isgx", O_RDWR);
    if (-1 == fd) {
        SE_TRACE(SE_TRACE_WARNING, "open isgx device failed\n");
        return false;
    }
    m_hdevice = fd;

    return true;
}

void EnclaveCreatorHW::close_se_device()
{
    LockGuard lock(&m_dev_mutex);

    if (m_hdevice != -1)
    {
        close(m_hdevice);
        m_hdevice = -1;
    }
}
