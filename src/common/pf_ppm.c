/*********************************************************************
 *        _       _         _
 *  _ __ | |_  _ | |  __ _ | |__   ___
 * | '__|| __|(_)| | / _` || '_ \ / __|
 * | |   | |_  _ | || (_| || |_) |\__ \
 * |_|    \__|(_)|_| \__,_||_.__/ |___/
 *
 * www.rt-labs.com
 * Copyright 2018 rt-labs AB, Sweden.
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 ********************************************************************/

/**
 * @file
 * @brief Implements the Cyclic Provider Protocol Machine (PPM)
 *
 * This handles cyclic sending of data (and IOPS). Initalizes transmit buffers.
 *
 * One instance of PPM exist per CR (together with a CPM).
 *
 * The states are W_START and RUN.
 *
 * There are functions used by the application (via pnet_api.c) to set and get
 * data, IOCS and IOPS.
 *
 * A global mutex is used instead of a per-instance mutex.
 * The locking time is very low so it should not be very congested.
 * The mutex is created on the first call to pf_ppm_create and deleted on
 * the last call to pf_ppm_close.
 * Keep track of how many instances exist and delete the mutex when the
 * number reaches 0 (zero).
 *
 */


#ifdef UNIT_TEST
#define os_eth_send mock_os_eth_send
#endif

#include <string.h>
#include "pf_includes.h"

#if PNET_OS_RTOS_SUPPORTED == 0
static const char          *ppm_sync_name = "ppm";
#endif

void pf_ppm_init(
   pnet_t                  *net)
{
   net->ppm_instance_cnt = ATOMIC_VAR_INIT(0);
}

/********************* Error handling ****************************************/

/**
 * @internal
 * Send error indications to other components.
 * @param net              InOut: The p-net stack instance
 * @param p_ar             InOut: The AR instance.
 * @param p_ppm            In:   The PPM instance.
 * @param error            In:   An error flag.
 * @return  0  always.
 */
static int pf_ppm_state_ind(
   pnet_t                  *net,
   pf_ar_t                 *p_ar,
   pf_ppm_t                *p_ppm,
   bool                    error)
{
   if (error == true)
   {
      p_ar->err_cls = PNET_ERROR_CODE_1_PPM;
      p_ar->err_code = PNET_ERROR_CODE_2_PPM_INVALID;
      pf_cmsu_ppm_error_ind(net, p_ar, p_ar->err_cls, p_ar->err_code);
   }

   return 0;
}


/******************** Start, stop, send, state *******************************/

/**
 * @internal
 * Handle state changes in the PPM instance.
 * @param p_ppm            InOut: The PPM instance.
 * @param state            In:   The new PPM state.
 */
static void pf_ppm_set_state(
   pf_ppm_t                *p_ppm,
   pf_ppm_state_values_t   state)
{
   LOG_DEBUG(PF_PPM_LOG, "PPM(%d): New state %d\n", __LINE__, state);
   p_ppm->state = state;
}

/**
 * @internal
 * Initialize a transmit buffer of a PPM instance.
 *
 * Insert destination and source MAC addresses, VLAN tag, Ethertype and
 * Profinet frame ID.
 *
 * Initialize the rest of the buffer to zero.
 *
 * @param p_ppm            In:   The PPM instance.
 * @param p_buf            InOut:The buffer to be initialized.
 * @param frame_id         In:   The frame_id.
 * @param p_header         In:   The VLAN tag header IOCR parameter.
 */
static void pf_ppm_init_buf(
   pf_ppm_t                *p_ppm,
   os_buf_t                *p_buf,
   uint16_t                frame_id,
   pf_iocr_tag_header_t    *p_header)
{
   uint16_t                pos;
   uint8_t                 *p_payload = (uint8_t*)p_buf->payload;
   uint16_t                u16 = 0;

   p_buf->len = p_ppm->buffer_length;
   memset(p_payload, 0, p_buf->len);
   pos = 0;

   /* Insert destination MAC address */
   memcpy(&p_payload[pos], &p_ppm->da, sizeof(p_ppm->da));
   pos += sizeof(p_ppm->da);

   /* Insert source MAC address */
   memcpy(&p_payload[pos], &p_ppm->sa, sizeof(p_ppm->sa));
   pos += sizeof(p_ppm->sa);

   /* Insert VLAN Tag protocol identifier (TPID) */
   u16 = OS_ETHTYPE_VLAN;
   u16 = htons(u16);
   memcpy(&p_payload[pos], &u16, sizeof(u16));
   pos += sizeof(u16);

   /* Insert VLAN ID (VID) and priority (Priority Code Point = PCP) */
   u16 = p_header->vlan_id & 0x0FFF;
   u16 |= (p_header->iocr_user_priority & 0x0007) << 13;  /* Three leftmost bits */
   u16 = htons(u16);
   memcpy(&p_payload[pos], &u16, sizeof(u16));
   pos += sizeof(u16);

   /* Insert EtherType */
   u16 = OS_ETHTYPE_PROFINET;
   u16 = htons(u16);
   memcpy(&p_payload[pos], &u16, sizeof(u16));
   pos += sizeof(u16);

   /* Insert Profinet frame ID (first part of Ethernet frame payload) */
   u16 = htons(frame_id);
   memcpy(&p_payload[pos], &u16, sizeof(u16));
   /* No further pos advancement, to suppress clang warning */
}

/**
 * @internal
 * Finalize a PPM transmit message in the send buffer.
 *
 * Insert data, cycle counter, data status and transfer status.
 *
 * @param net              InOut: The p-net stack instance
 * @param p_ppm            In:   The PPM instance.
 * @param data_length      In:   The length of the message.
 */
static void pf_ppm_finish_buffer(
   pnet_t                  *net,
   pf_ppm_t                *p_ppm,
   uint16_t                data_length)
{
   uint8_t                 *p_payload = ((os_buf_t*)p_ppm->p_send_buffer)->payload;
   uint16_t                u16;
   uint16_t				   _remainder = 0,
		   	   	   	   	   _ratio = 0;
   int32_t                 cycle_tmp = os_get_current_time_us()*4;

   cycle_tmp	= cycle_tmp/125;           /* Cycle counter. Get 4/125 = 31.25us tics */
   _ratio 		= p_ppm->send_clock_factor * p_ppm->reduction_ratio;
   _remainder	= cycle_tmp%_ratio;
   
   /* Make the cycle counter a multiple of send_clock_factor * reduction ration 
    * by subtracting the remainder in the calculations above 
    */
   if(cycle_tmp<_ratio)
	   cycle_tmp = _ratio;
   else
	   cycle_tmp = cycle_tmp - _remainder; 
   
   p_ppm->cycle = cycle_tmp;           /* Cycle counter corrected! */

   /*logMsg("Inteval: %u \n\r", p_ppm->control_interval,2,3,4,5,6);*/
   
   u16 = htons(p_ppm->cycle);

   /* Insert data */
   os_mutex_lock(net->ppm_buf_lock);
   memcpy(&p_payload[p_ppm->buffer_pos], p_ppm->buffer_data, data_length);
   os_mutex_unlock(net->ppm_buf_lock);

   /* Insert cycle counter */
   memcpy(&p_payload[p_ppm->cycle_counter_offset], &u16, sizeof(u16));

   /* Insert data status */
   memcpy(&p_payload[p_ppm->data_status_offset], &p_ppm->data_status, sizeof(p_ppm->data_status));

   /* Insert transfer status */
   memcpy(&p_payload[p_ppm->transfer_status_offset], &p_ppm->transfer_status, sizeof(p_ppm->transfer_status));
}

/**
 * @internal
 * Calculate the delay to use with the scheduler, by taking the stack cycle time into account.
 *
 * With a stack cycle time of 1 ms, a scheduled delay of 0-700 microseconds will
 * cause a nice periodicity of 1 ms. A scheduled delay of 1000 microseconds will
 * sometimes fire at next cycle, sometimes not. This gives an event spacing of
 * 1 or 2 ms.
 *
 * Similarly a scheduled delay of 1100 to 1700 microseconds causes
 * a nice periodicity of 2 ms, and a scheduled delay of 2100 to 2700
 * microseconds causes a nice periodicity of 3 ms. These measurements were
 * made using a Ubuntu Laptop.
 *
 * Scheduling a delay close to a multiple of the stack cycle time is risky, and
 * should be avoided. Calculate the number of stack cycles to wait, and
 * calculate a delay corresponding to half a cycle less.
 *
 * If the requested delay is in the range 1.5 to 2.5 stack cycle times, this
 * function will return a calculated delay giving a periodicity of 2 stack cycle
 * times. If the requested time is less than 1.5 stack cycle times, the
 * resulting periodicity is 1 stack cycle time.
 *
 * Note that this function calculates the delay time required to make the
 * scheduler fire at a specific stack tick. However the time jitter in the
 * firing is largely dependent on the underlaying operating system's ablitity to
 * trigger the stack execution with a high time precision.
 *
 * @param wanted_delay     In:    Wanted delay in microseconds.
 * @param stack_cycle_time In:    Stack cycle time in microseconds. Must be larger than 0.
 * @return Number of microseconds of delay to use with the scheduler.
 */
uint32_t pf_ppm_calculate_compensated_delay(
   uint32_t                wanted_delay,
   uint32_t                stack_cycle_time
)
{
   uint32_t                number_of_stack_tics = 1;  /* We must wait at least one tick */

   if (wanted_delay > stack_cycle_time + stack_cycle_time / 2)
   {
      number_of_stack_tics = (wanted_delay + stack_cycle_time / 2) / stack_cycle_time;
   }

   CC_ASSERT(number_of_stack_tics >= 1);
   CC_ASSERT(number_of_stack_tics < 0x80000000);  /* No rollover to 'negative' numbers */
#if PNET_OS_RTOS_SUPPORTED
   return number_of_stack_tics * stack_cycle_time;
#else
   return number_of_stack_tics * stack_cycle_time - stack_cycle_time / 2;
#endif
}

/**
 * @internal
 * Send the PPM data message to the controller.
 *
 * This is a callback for the scheduler. Arguments should fulfill pf_scheduler_timeout_ftn_t
 *
 * If the PPM has not been stopped during the wait, then a data message
 * is sent and the function is rescheduled.
 *
 * @param net              InOut: The p-net stack instance
 * @param arg              In:    The IOCR instance.
 * @param current_time     In:    The current time (system time in microseconds).
 */
static void pf_ppm_send(
   pnet_t                  *net,
   void                    *arg,
   uint32_t                current_time)
{
   pf_iocr_t               *p_arg = (pf_iocr_t *)arg;
   int                     ret = -1;
   p_arg->ppm.ci_timer = UINT32_MAX;
   if (p_arg->ppm.ci_running == true)
   {
      /* Insert data, status etc. The in_length is the size of input to the controller */
      pf_ppm_finish_buffer(net, &p_arg->ppm, p_arg->in_length);

      /* Send the Ethernet frame */
      /* ToDo: Handle RT_CLASS_UDP */
      if (os_eth_send(p_arg->p_ar->p_sess->eth_handle, p_arg->ppm.p_send_buffer) <= 0)
      {
    	  net->interface_statistics.ifOutErrors++;
         LOG_ERROR(PF_PPM_LOG, "PPM(%d): Error from os_eth_send(ppm)\n", __LINE__);
      }
      else
      {
    	  net->interface_statistics.ifOutOctects++;
#if PNET_OS_RTOS_SUPPORTED
      /*Santiy Check*/
      if(NULL != p_arg->ppm.rt_args->rt_timer)
      {
    	  /*Start the timer */
    	   os_timer_start(p_arg->ppm.rt_args->rt_timer);
    	   ret = 0;
      }
#else
      /* Schedule next execution */
 	  ret = pf_scheduler_add(net, p_arg->ppm.compensated_control_interval, ppm_sync_name, pf_ppm_send, arg, &p_arg->ppm.ci_timer);
#endif
         if(ret == 0)
         {
            p_arg->ppm.trx_cnt++;
            if (p_arg->ppm.first_transmit == false)
            {
               pf_ppm_state_ind(net, p_arg->p_ar, &p_arg->ppm, false);   /* No error */
               p_arg->ppm.first_transmit = true;
            }
         }
         else
         {
            p_arg->ppm.ci_timer = UINT32_MAX;
            pf_ppm_state_ind(net, p_arg->p_ar, &p_arg->ppm, true);       /* Error */
         }
      }
   }
}

#if PNET_OS_RTOS_SUPPORTED
static void pf_ppm_wdtimer_event(os_timer_t *timer)
{
	pf_ppm_t			*p_ppm 			= (pf_ppm_t*)timer->arg;
	pf_ppm_rt_args_t 	*p_ppm_rt_args	= p_ppm->rt_args;
	
	p_ppm_rt_args->cb( p_ppm_rt_args->net, p_ppm_rt_args->arg, p_ppm->ci_timer);
}
#endif

int pf_ppm_activate_req(
   pnet_t                  *net,
   pf_ar_t                 *p_ar,
   uint32_t                crep)
{
   int                     ret = -1;
   const uint16_t          vlan_size = 4;
   pf_iocr_t               *p_iocr = &p_ar->iocrs[crep];
   pf_ppm_t                *p_ppm;
   uint32_t                cnt;

   cnt = atomic_fetch_add(&net->ppm_instance_cnt, 1);
   if (cnt == 0)
   {
      net->ppm_buf_lock = os_mutex_create();
   }

   p_ppm = &p_iocr->ppm;
   if (p_ppm->state == PF_PPM_STATE_RUN)
   {
      p_ar->err_cls = PNET_ERROR_CODE_1_PPM;
      p_ar->err_code = PNET_ERROR_CODE_2_PPM_INVALID_STATE;
   }
   else
   {
      p_ppm->first_transmit = false;

      memcpy(&p_ppm->sa, &p_ar->ar_result.cm_responder_mac_add, sizeof(p_ppm->sa));
      memcpy(&p_ppm->da, &p_ar->ar_param.cm_initiator_mac_add, sizeof(p_ppm->da));

      p_ppm->buffer_pos = 2*sizeof(pnet_ethaddr_t) + vlan_size + sizeof(uint16_t) + sizeof(uint16_t);
      p_ppm->cycle = 0;
      p_ppm->transfer_status = 0;

      /* Pre-compute some offsets into the send buffer */
      p_ppm->cycle_counter_offset = p_ppm->buffer_pos +           /* ETH frame header */
            p_iocr->param.c_sdu_length;                           /* Profinet data length */

      p_ppm->data_status_offset = p_ppm->buffer_pos +             /* ETH frame header */
            p_iocr->param.c_sdu_length +                          /* Profinet data length */
            sizeof(uint16_t);                                     /* cycle counter */

      p_ppm->transfer_status_offset = p_ppm->buffer_pos +         /* ETH frame header */
            p_iocr->param.c_sdu_length +                          /* Profinet data length */
            sizeof(uint16_t) +                                    /* cycle counter */
            1;                                                    /* data status */

      p_ppm->buffer_length = p_ppm->buffer_pos +                  /* ETH frame header */
            p_iocr->param.c_sdu_length +                          /* Profinet data length */
            sizeof(uint16_t) +                                    /* cycle counter */
            1 +                                                   /* data status */
            1;                                                    /* transfer status */

      p_ppm->data_status = BIT(PNET_DATA_STATUS_BIT_STATE) +                 /* PRIMARY */
                      BIT(PNET_DATA_STATUS_BIT_DATA_VALID) +
                      BIT(PNET_DATA_STATUS_BIT_STATION_PROBLEM_INDICATOR);   /* Normal */

      /* Get the buffer to store the outgoing frame into. */
      p_ppm->p_send_buffer = os_buf_alloc(PF_FRAME_BUFFER_SIZE);

      /* Default_values: Set buffer to zero and IOxS to BAD (=0) */
      /* Default_status: Set cycle_counter to invalid, transfer_status = 0, data_status = 0 */
      pf_ppm_init_buf(p_ppm, p_ppm->p_send_buffer,
         p_iocr->param.frame_id,
         &p_iocr->param.iocr_tag_header);

      p_ppm->control_interval = ((uint32_t)p_iocr->param.send_clock_factor *
            (uint32_t)p_iocr->param.reduction_ratio * 1000U) / 32U;   /* us */
      
      /*Keep history of this as we will need it for counter calculations */
      p_ppm->send_clock_factor = p_iocr->param.send_clock_factor;
      p_ppm->reduction_ratio = p_iocr->param.reduction_ratio;

      p_ppm->compensated_control_interval = pf_ppm_calculate_compensated_delay(
         p_ppm->control_interval,
         net->scheduler_tick_interval);

      LOG_DEBUG(PF_PPM_LOG, "PPM(%d): Starting cyclic sending for CREP %u with period %u microseconds\n",
         __LINE__, crep, p_ppm->control_interval);

      pf_ppm_set_state(p_ppm, PF_PPM_STATE_RUN);

      p_ppm->ci_running = true;

#if PNET_OS_RTOS_SUPPORTED
      /*Implement a more deterministic IO timer to send data */
      p_ppm->rt_args->net = net;
      p_ppm->rt_args->cb = pf_ppm_send;
      p_ppm->rt_args->arg = p_iocr;
      
      /*Create the timer*/
	  p_ppm->rt_args->rt_timer = os_timer_create(
			  net->interrupt_timer_handle,			/* interrupt handle */
			  p_ppm->compensated_control_interval,	/* Send interval */
			  pf_ppm_wdtimer_event,					/* function pointer */
			  (void*)p_ppm,							/* argument */
			  false);								/* oneshot */

      /*Santiy Check*/
      if(NULL != p_ppm->rt_args->rt_timer)
      {
    	  /*Start the timer */
    	   os_timer_start(p_ppm->rt_args->rt_timer);
    	   ret = 0;
      }
      else
      {
          LOG_DEBUG(PF_PPM_LOG, "PPM(%d): Realtime time was not created!\n",__LINE__);
    	  ret = -1;
      }
#else
      ret = pf_scheduler_add(net, p_ppm->compensated_control_interval,
         ppm_sync_name, pf_ppm_send, p_iocr, &p_ppm->ci_timer);
#endif
      if (ret != 0)
      {
         p_ppm->ci_timer = UINT32_MAX;
         pf_ppm_state_ind(net, p_ar, p_ppm, true);          /* Error */
      }
   }

   return ret;
}

int pf_ppm_close_req(
   pnet_t                  *net,
   pf_ar_t                 *p_ar,
   uint32_t                crep)
{
   pf_ppm_t                *p_ppm;
   uint32_t                cnt;

   LOG_DEBUG(PF_PPM_LOG, "PPM(%d): close\n", __LINE__);
   p_ppm = &p_ar->iocrs[crep].ppm;
   p_ppm->ci_running = false;
#if PNET_OS_RTOS_SUPPORTED
   /* Stop the timer */
   if(NULL != p_ppm->rt_args->rt_timer)
   {
	   p_ppm->rt_args->rt_timer->exit = true;
	   os_timer_destroy(p_ppm->rt_args->rt_timer);
	   p_ppm->ci_timer = UINT32_MAX;
	   p_ppm->rt_args->rt_timer->timerid = 0;
   }
#else
   if (p_ppm->ci_timer != UINT32_MAX)
   {
      pf_scheduler_remove(net, ppm_sync_name, p_ppm->ci_timer);
      p_ppm->ci_timer = UINT32_MAX;
   }
#endif

   os_buf_free(p_ppm->p_send_buffer);
   pf_ppm_set_state(p_ppm, PF_PPM_STATE_W_START);

   cnt = atomic_fetch_sub(&net->ppm_instance_cnt, 1);
   if (cnt == 1)
   {
      os_mutex_destroy(net->ppm_buf_lock);
      net->ppm_buf_lock = NULL;

      p_ppm->data_status = 0;
   }

   return 0;
}

/**
 * @internal
 * Find the AR, input IOCR and IODATA object instances for the specified sub-slot.
 * @param net              InOut: The p-net stack instance
 * @param api_id           In:   The API id.
 * @param slot_nbr         In:   The slot number.
 * @param subslot_nbr      In:   The sub-slot number.
 * @param pp_ar            Out:  The AR instance.
 * @param pp_iocr          Out:  The IOCR instance.
 * @param pp_iodata        Out:  The IODATA object instance.
 * @return  0  If the information has been found.
 *          -1 If the information was not found.
 */
static int pf_ppm_get_ar_iocr_desc(
   pnet_t                  *net,
   uint32_t                api_id,
   uint16_t                slot_nbr,
   uint16_t                subslot_nbr,
   pf_ar_t                 **pp_ar,
   pf_iocr_t               **pp_iocr,
   pf_iodata_object_t      **pp_iodata)
{
   int                     ret = -1;
   uint32_t                crep;
   uint16_t                iodata_ix;
   pf_subslot_t            *p_subslot = NULL;
   pf_ar_t                 *p_ar = NULL;
   pf_iocr_t               *p_iocr = NULL;
   bool                    found = false;

   if (pf_cmdev_get_subslot_full(net, api_id, slot_nbr, subslot_nbr, &p_subslot) == 0)
   {
      p_ar = p_subslot->p_ar;
   }

   if (p_ar == NULL)
   {
      LOG_DEBUG(PF_PPM_LOG, "PPM(%d): No AR set in sub-slot\n", __LINE__);
   }
   else
   {
      /*
       * Search the AR for an INPUT CR or an MC provider CR containing the
       * sub-slot.
       */
      for (crep = 0; ((found == false) && (crep < p_ar->nbr_iocrs)); crep++)
      {
         if ((p_ar->iocrs[crep].param.iocr_type == PF_IOCR_TYPE_INPUT) ||
             (p_ar->iocrs[crep].param.iocr_type == PF_IOCR_TYPE_MC_PROVIDER))
         {
            p_iocr = &p_ar->iocrs[crep];
            for (iodata_ix = 0; ((found == false) && (iodata_ix < p_iocr->nbr_data_desc)); iodata_ix++)
            {
               if ((p_iocr->data_desc[iodata_ix].in_use == true) &&
                   (p_iocr->data_desc[iodata_ix].api_id == api_id) &&
                   (p_iocr->data_desc[iodata_ix].slot_nbr == slot_nbr) &&
                   (p_iocr->data_desc[iodata_ix].subslot_nbr == subslot_nbr))
               {
                  *pp_iodata = &p_iocr->data_desc[iodata_ix];
                  *pp_iocr = p_iocr;
                  *pp_ar = p_ar;
                  found = true;
               }
            }
         }
      }
   }

   if (found == true)
   {
      ret = 0;
   }

   return ret;
}

/**************** Set and get data, IOPS and IOCS ****************************/

int pf_ppm_set_data_and_iops(
   pnet_t                  *net,
   uint32_t                api_id,
   uint16_t                slot_nbr,
   uint16_t                subslot_nbr,
   uint8_t                 *p_data,
   uint16_t                data_len,
   uint8_t                 *p_iops,
   uint8_t                 iops_len)
{
   int                     ret = -1;
   pf_iocr_t               *p_iocr = NULL;
   pf_iodata_object_t      *p_iodata = NULL;
   pf_ar_t                 *p_ar = NULL;

   if (pf_ppm_get_ar_iocr_desc(net, api_id, slot_nbr, subslot_nbr, &p_ar, &p_iocr, &p_iodata) == 0)
   {
      switch (p_iocr->ppm.state)
      {
      case PF_PPM_STATE_W_START:
         p_ar->err_cls = PNET_ERROR_CODE_1_PPM;
         p_ar->err_code = PNET_ERROR_CODE_2_PPM_INVALID_STATE;
         LOG_DEBUG(PF_PPM_LOG, "PPM(%d): Set data in wrong state: %u\n", __LINE__, p_iocr->ppm.state);
         break;
      case PF_PPM_STATE_RUN:
         if ((data_len == p_iodata->data_length) && (iops_len == p_iodata->iops_length))
         {
            CC_ASSERT(net->ppm_buf_lock != NULL);
            os_mutex_lock(net->ppm_buf_lock);
            if (data_len > 0)
            {
               memcpy(&p_iocr->ppm.buffer_data[p_iodata->data_offset], p_data, data_len);
            }
            if (iops_len > 0)
            {
               memcpy(&p_iocr->ppm.buffer_data[p_iodata->iops_offset], p_iops, iops_len);
            }
            os_mutex_unlock(net->ppm_buf_lock);

            p_iodata->data_avail = true;
            ret = 0;
         }
         else
         {
            LOG_ERROR(PF_PPM_LOG, "PPM(%d): data_len, iops_len %u %u expected lengths %u %u\n", __LINE__,
               data_len, iops_len, p_iodata->data_length, p_iodata->iops_length);
         }
         break;
      default:
         LOG_ERROR(PF_PPM_LOG, "PPM(%d): Set data in wrong state: %u\n", __LINE__, p_iocr->ppm.state);
         break;
      }
   }
   else
   {
      /* May happen after an ABORT */
      LOG_DEBUG(PF_PPM_LOG, "PPM(%d): No data descriptor found for set data\n", __LINE__);
   }

   return ret;
}

int pf_ppm_set_iocs(
   pnet_t                  *net,
   uint32_t                api_id,
   uint16_t                slot_nbr,
   uint16_t                subslot_nbr,
   uint8_t                 *p_iocs,
   uint8_t                 iocs_len)
{
   int                     ret = -1;
   pf_iocr_t               *p_iocr = NULL;
   pf_iodata_object_t      *p_iodata = NULL;
   pf_ar_t                 *p_ar = NULL;

   if (pf_ppm_get_ar_iocr_desc(net, api_id, slot_nbr, subslot_nbr, &p_ar, &p_iocr, &p_iodata) == 0)
   {
      switch (p_iocr->ppm.state)
      {
      case PF_PPM_STATE_W_START:
         p_ar->err_cls = PNET_ERROR_CODE_1_PPM;
         p_ar->err_code = PNET_ERROR_CODE_2_PPM_INVALID_STATE;
         LOG_DEBUG(PF_PPM_LOG, "PPM(%d): Set iocs in wrong state: %u\n", __LINE__, p_iocr->ppm.state);
         break;
      case PF_PPM_STATE_RUN:
         if (iocs_len == p_iodata->iocs_length)
         {
            CC_ASSERT(net->ppm_buf_lock != NULL);
            os_mutex_lock(net->ppm_buf_lock);
            memcpy(&p_iocr->ppm.buffer_data[p_iodata->iocs_offset], p_iocs, iocs_len);
            os_mutex_unlock(net->ppm_buf_lock);

            ret = 0;
         }
         else if (p_iodata->iocs_length == 0)
         {
            /* ToDo: What does the spec say about this case? */
            LOG_DEBUG(PF_PPM_LOG, "PPM(%d): iocs_len is zero\n", __LINE__);
            ret = 0;
         }
         else
         {
            LOG_ERROR(PF_PPM_LOG, "PPM(%d): iocs_len %u expected length %u\n", __LINE__, (unsigned)p_iodata->iocs_length, (unsigned)sizeof(uint8_t));
         }
         break;
      default:
         LOG_ERROR(PF_PPM_LOG, "PPM(%d): Set data in wrong state: %u\n", __LINE__, (unsigned)p_iocr->ppm.state);
         break;
      }
   }
   else
   {
      /* May happen after an ABORT */
      LOG_ERROR(PF_PPM_LOG, "PPM(%d): No data descriptor found for set iocs\n", __LINE__);
   }

   return ret;
}

int pf_ppm_get_data_and_iops(
   pnet_t                  *net,
   uint32_t                api_id,
   uint16_t                slot_nbr,
   uint16_t                subslot_nbr,
   uint8_t                 *p_data,
   uint16_t                *p_data_len,
   uint8_t                 *p_iops,
   uint8_t                 *p_iops_len)
{
   int                     ret = -1;
   pf_iocr_t               *p_iocr = NULL;
   pf_iodata_object_t      *p_iodata = NULL;
   pf_ar_t                 *p_ar = NULL;

   if (pf_ppm_get_ar_iocr_desc(net, api_id, slot_nbr, subslot_nbr, &p_ar, &p_iocr, &p_iodata) == 0)
   {
      switch (p_iocr->ppm.state)
      {
      case PF_PPM_STATE_W_START:
         p_ar->err_cls = PNET_ERROR_CODE_1_PPM;
         p_ar->err_code = PNET_ERROR_CODE_2_PPM_INVALID_STATE;
         LOG_DEBUG(PF_PPM_LOG, "PPM(%d): Get data in wrong state: %u\n", __LINE__, p_iocr->ppm.state);
         break;
      case PF_PPM_STATE_RUN:
         if ((*p_data_len >= p_iodata->data_length) && (*p_iops_len >= p_iodata->iops_length))
         {
            CC_ASSERT(net->ppm_buf_lock != NULL);
            os_mutex_lock(net->ppm_buf_lock);
            memcpy(p_data, &p_iocr->ppm.buffer_data[p_iodata->data_offset], p_iodata->data_length);
            memcpy(p_iops, &p_iocr->ppm.buffer_data[p_iodata->iops_offset], p_iodata->iops_length);
            os_mutex_unlock(net->ppm_buf_lock);

            *p_data_len = p_iodata->data_length;
            *p_iops_len = (uint8_t)p_iodata->iops_length;
            ret = 0;
         }
         else
         {
            LOG_ERROR(PF_PPM_LOG, "PPM(%d): data_len %u iops_len %u expected lengths %u %u\n", __LINE__,
               (unsigned)*p_data_len, (unsigned)*p_iops_len, (unsigned)p_iodata->data_length, (unsigned)p_iodata->iops_length);
         }
         break;
      default:
         LOG_ERROR(PF_PPM_LOG, "PPM(%d): Get data in wrong state: %u\n", __LINE__, (unsigned)p_iocr->ppm.state);
         break;
      }
   }
   else
   {
      /* May happen after an ABORT */
      LOG_ERROR(PF_PPM_LOG, "PPM(%d): No data descriptor found for get data\n", __LINE__);
   }

   return ret;
}

/**
 * Retrieve IOCS for a sub-module.
 * @param net              InOut: The p-net stack instance
 * @param api_id           In:   The API id.
 * @param slot_nbr         In:   The slot number.
 * @param subslot_nbr      In:   The sub-slot number.
 * @param p_iocs           Out:  The IOCS of the application data.
 * @param p_iocs_len       In:   Size of buffer at p_iocs.
 *                         Out:  Actual size of IOCS data.
 * @return  0  if the IOCS could be retrieved.
 *          -1 if an error occurred.
 */
int pf_ppm_get_iocs(
   pnet_t                  *net,
   uint32_t                api_id,
   uint16_t                slot_nbr,
   uint16_t                subslot_nbr,
   uint8_t                 *p_iocs,
   uint8_t                 *p_iocs_len)
{
   int                     ret = -1;
   pf_iocr_t               *p_iocr = NULL;
   pf_iodata_object_t      *p_iodata = NULL;
   pf_ar_t                 *p_ar = NULL;

   if (pf_ppm_get_ar_iocr_desc(net, api_id, slot_nbr, subslot_nbr, &p_ar, &p_iocr, &p_iodata) == 0)
   {
      switch (p_iocr->ppm.state)
      {
      case PF_PPM_STATE_W_START:
         p_ar->err_cls = PNET_ERROR_CODE_1_PPM;
         p_ar->err_code = PNET_ERROR_CODE_2_PPM_INVALID_STATE;
         LOG_DEBUG(PF_PPM_LOG, "PPM(%d): Get iocs in wrong state: %u\n", __LINE__, p_iocr->ppm.state);
         break;
      case PF_PPM_STATE_RUN:
         if (*p_iocs_len >= p_iodata->iocs_length)
         {
            CC_ASSERT(net->ppm_buf_lock != NULL);
            os_mutex_lock(net->ppm_buf_lock);
            memcpy(p_iocs, &p_iocr->ppm.buffer_data[p_iodata->iocs_offset], p_iodata->iocs_length);
            os_mutex_unlock(net->ppm_buf_lock);

            *p_iocs_len = (uint8_t)p_iodata->iocs_length;
            ret = 0;
         }
         else
         {
            LOG_ERROR(PF_PPM_LOG, "PPM(%d): iocs_len %u expected length %u\n", __LINE__, (unsigned)*p_iocs_len, (unsigned)p_iodata->iocs_length);
         }
         break;
      default:
         LOG_ERROR(PF_PPM_LOG, "PPM(%d): Get iocs in wrong state: %u\n", __LINE__, (unsigned)p_iocr->ppm.state);
         break;
      }
   }
   else
   {
      /* May happen after an ABORT */
      LOG_ERROR(PF_PPM_LOG, "PPM(%d): No data descriptor found for get iocs\n", __LINE__);
   }

   return ret;
}

/****************************** Data status **********************************/

int pf_ppm_set_data_status_state(
   pf_ar_t                 *p_ar,
   uint32_t                crep,
   bool                    primary)
{
   pf_ppm_t                *p_ppm;

   p_ppm = &p_ar->iocrs[crep].ppm;

   if (primary == true)
   {
      p_ppm->data_status |= BIT(PNET_DATA_STATUS_BIT_STATE);
   }
   else
   {
      p_ppm->data_status &= ~BIT(PNET_DATA_STATUS_BIT_STATE);
   }
   return 0;
}

int pf_ppm_set_data_status_redundancy(
   pf_ar_t                 *p_ar,
   uint32_t                crep,
   bool                    redundant)
{
   pf_ppm_t                *p_ppm;

   p_ppm = &p_ar->iocrs[crep].ppm;

   if (redundant == true)
   {
      p_ppm->data_status |= BIT(PNET_DATA_STATUS_BIT_REDUNDANCY);
   }
   else
   {
      p_ppm->data_status &= ~BIT(PNET_DATA_STATUS_BIT_REDUNDANCY);
   }
   return 0;
}

int pf_ppm_set_data_status_provider(
   pf_ar_t                 *p_ar,
   uint32_t                crep,
   bool                    run)
{
   pf_ppm_t                *p_ppm;

   p_ppm = &p_ar->iocrs[crep].ppm;

   if (run == true)
   {
      p_ppm->data_status |= BIT(PNET_DATA_STATUS_BIT_PROVIDER_STATE);
   }
   else
   {
      p_ppm->data_status &= ~BIT(PNET_DATA_STATUS_BIT_PROVIDER_STATE);
   }
   return 0;
}

int pf_ppm_get_data_status(
   pf_ppm_t                *p_ppm,
   uint8_t                 *p_data_status)
{
   *p_data_status = p_ppm->data_status;

   return 0;
}

void pf_ppm_set_problem_indicator(
   pf_ar_t                 *p_ar,
   bool                    problem_indicator)
{
   uint16_t                ix;

   /* Save so it may be included in all data messages */

   for (ix = 0; ix < NELEMENTS(p_ar->iocrs); ix++)
   {
      if ((p_ar->iocrs[ix].param.iocr_type == PF_IOCR_TYPE_INPUT) ||
          (p_ar->iocrs[ix].param.iocr_type == PF_IOCR_TYPE_MC_PROVIDER))
      {
         if (problem_indicator == true)
         {
            p_ar->iocrs[ix].ppm.data_status &= ~BIT(5);
         }
         else
         {
            p_ar->iocrs[ix].ppm.data_status |= BIT(5);    /* OK */
         }
      }
   }
}


/**************** Diagnostic strings *****************************************/

/**
 * @internal
 * Return a string representation of the PPM state.
 * @param state            In:   The PPM state.
 * @return  A string representing the PPM state.
 */
static const char *pf_ppm_state_to_string(
   pf_ppm_state_values_t   state)
{
   const char *s = "<unknown>";

   switch (state)
   {
   case PF_PPM_STATE_W_START: s = "PF_PPM_STATE_W_START"; break;
   case PF_PPM_STATE_RUN:     s = "PF_PPM_STATE_RUN"; break;
   }

   return s;
}

void pf_ppm_show(
   pf_ppm_t                *p_ppm)
{
   printf("ppm:\n");
   printf("   state                        = %s\n", pf_ppm_state_to_string(p_ppm->state));
   printf("   errline                      = %u\n", (unsigned)p_ppm->errline);
   printf("   errcnt                       = %u\n", (unsigned)p_ppm->errcnt);
   printf("   first_transmit               = %u\n", (unsigned)p_ppm->first_transmit);
   printf("   trx_cnt                      = %u\n", (unsigned)p_ppm->trx_cnt);
   printf("   p_send_buffer                = %p\n", p_ppm->p_send_buffer);
   printf("   p_send_buffer->len           = %u\n", p_ppm->p_send_buffer ? ((os_buf_t *)(p_ppm->p_send_buffer))->len : 0);
   printf("   new_buf                      = %u\n", (unsigned)p_ppm->new_buf);
   printf("   control_interval             = %u\n", (unsigned)p_ppm->control_interval);
   printf("   compensated_control_interval = %u\n", (unsigned)p_ppm->compensated_control_interval);
   printf("   cycle                        = %u\n", (unsigned)p_ppm->cycle);
   printf("   cycle_counter_off            = %u\n", (unsigned)p_ppm->cycle_counter_offset);
   printf("   data_status_offset           = %u\n", (unsigned)p_ppm->data_status_offset);
   printf("   transfer_status_of           = %u\n", (unsigned)p_ppm->transfer_status_offset);
   printf("   ci_running                   = %u\n", (unsigned)p_ppm->ci_running);
   printf("   ci_timer                     = %u\n", (unsigned)p_ppm->ci_timer);
   printf("   transfer_status              = %u\n", (unsigned)p_ppm->transfer_status);
   printf("   data_status                  = %x\n", (unsigned)p_ppm->data_status);
   printf("   buffer_length                = %u\n", (unsigned)p_ppm->buffer_length);
   printf("   buffer_pos                   = %u\n", (unsigned)p_ppm->buffer_pos);
}
