/**
 * HydroZynq Data acquisition firmware.
 *
 * @author Ryan Summers
 * @date 10/17/2017
 */

#include "abort.h"
#include "adc.h"
#include "correlation_util.h"
#include "dma.h"
#include "lwip/ip.h"
#include "lwip/udp.h"
#include "network_stack.h"
#include "sample_util.h"
#include "spi.h"
#include "system.h"
#include "system_params.h"
#include "time_util.h"
#include "transmission_util.h"
#include "types.h"
#include "udp.h"
#include "db.h"

#include "adc_dma_addresses.h"

#include <string.h>

/**
 * The DMA engine for reading samples.
 */
dma_engine_t dma;

/**
 * The SPI driver for controlling the ADC.
 */
spi_driver_t adc_spi;

/**
 * The ADC driver to use for controlling the ADC parameters.
 */
adc_driver_t adc;

/**
 * The maximum number of samples for 2.2 seconds at 65Msps.
 */
#define MAX_SAMPLES 45000 * 2200

/**
 * The array of current samples.
 */
sample_t samples[MAX_SAMPLES];

/**
 * The array of correlation results for the cross correlation.
 */
correlation_t correlations[50000];

/**
 * Specifies that the stream is in debug mode and transmits extra information.
 */
bool debug_stream = false;

/**
 * Specifies that the ping has been synced on.
 */
bool sync = false;

/**
 * Specifies the current operating parameters of the application.
 */
HydroZynqParams params;

typedef struct KeyValuePair
{
    char *key;
    char *value;
} KeyValuePair;

/**
 * Defines the highpass IIR filter coefficients. These were generated by Matlab
 * using the designfilt function.
 */
filter_coefficients_t highpass_iir[5] = {
    {{0.976572753292004, -1.953145506584008, 0.976572753292004,
        1.000000000000000, -1.998354115074282, 0.998926104509836}},
    {{0.975206721477597, -1.950413442955194, 0.975206721477597,
        1.000000000000000, -1.995495119158081, 0.996193697294377}},
    {{0.972451482822301, -1.944902965644602, 0.972451482822301,
        1.000000000000000, -1.989660620860693, 0.990750529959661}},
    {{0.963669622248601, -1.927339244497202, 0.963669622248601,
        1.000000000000000, -1.970992420143032, 0.973473065140308}},
    {{0.906313647059524, -1.812627294119048, 0.906313647059524,
        1.000000000000000, -1.848974099452832, 0.860723515924862}}};

/**
 * Parses an argument packet into key-value pairs.
 *
 * @param data The data to parse.
 * @param len The length of the memory pointed to by data.
 * @param[out] pairs A list of key-value pairs to store the parsed results in.
 * @param max_pairs The maximum number of key-value pairs allowed.
 * @param[out] num_pairs The number of key-value pairs found in the string.
 *
 * @return Success or fail.
 */
result_t parse_packet(char *data,
                      const size_t len,
                      KeyValuePair *pairs,
                      const size_t max_pairs,
                      size_t *num_pairs)
{
    AbortIfNot(data, fail);
    AbortIfNot(pairs, fail);
    AbortIfNot(num_pairs, fail);

    *num_pairs = 0;
    char *last_start = data;
    for (int i = 0; i < len && *num_pairs < max_pairs; ++i)
    {
        if (data[i] == ',' || data[i] == 0)
        {
            pairs[*num_pairs].key = last_start;
            data[i] = 0;
            (*num_pairs) = *num_pairs + 1;
            last_start = &(data[i + 1]);
        }

        if (data[i] == 0)
        {
            break;
        }
    }

    for (int i = 0; i < *num_pairs; ++i)
    {
        pairs[i].value = NULL;
        for (int j = 0; j < strlen(pairs[i].key); ++j)
        {
            if (pairs[i].key[j] == ':')
            {
                pairs[i].key[j] = 0;
                pairs[i].value = &(pairs[i].key[j + 1]);
                break;
            }
        }

        AbortIfNot(pairs[i].value, fail);
    }

    return success;
}

/**
 * Callback for receiving a UDP packet.
 *
 * @return None.
 */
void receive_command(void *arg, struct udp_pcb *upcb, struct pbuf *p, struct ip_addr *addr, uint16_t port)
{
    KeyValuePair pairs[10];
    size_t num_entries = 0;

    char data[1024];
    if (p->len > (sizeof(data) - 1))
    {
        pbuf_free(p);
        dbprintf("Packet too long! Length was %d but internal buffer is %d\n",
                p->len, sizeof(data));
        return;
    }

    memcpy(data, p->payload, p->len);
    pbuf_free(p);
    data[p->len] = 0;

    AbortIfNot(parse_packet(data, p->len + 1, pairs, 10, &num_entries), );

    for (int i = 0; i < num_entries; ++i)
    {
        dbprintf("Key: '%s' Value: '%s'\n", pairs[i].key, pairs[i].value);
        if (strcmp(pairs[i].key, "threshold") == 0)
        {
            unsigned int threshold;
            AbortIfNot(sscanf(pairs[i].value, "%u", &threshold), );
            params.ping_threshold = threshold;
            sync = false;
            dbprintf("Ping threshold has been set to %d\n", params.ping_threshold);
        }
        else if (strcmp(pairs[i].key, "filter") == 0)
        {
            unsigned int debug = 0;
            AbortIfNot(sscanf(pairs[i].value, "%u", &debug), );
            params.filter = (debug == 0)? false : true;
            dbprintf("Filtering is: %s\n",
                    (debug_stream)? "Enabled" : "Disabled");
        }
        else if (strcmp(pairs[i].key, "debug") == 0)
        {
            unsigned int debug = 0;
            AbortIfNot(sscanf(pairs[i].value, "%u", &debug), );
            debug_stream = (debug == 0)? false : true;
            dbprintf("Debug stream is: %s\n",
                    (debug_stream)? "Enabled" : "Disabled");
        }
        else if (strcmp(pairs[i].key, "pre_ping_duration_us") == 0)
        {
            unsigned int duration = 0;
            AbortIfNot(sscanf(pairs[i].value, "%u", &duration), );

            params.pre_ping_duration = micros_to_ticks(duration);
            dbprintf("Pre-ping duration is %u us.\n", duration);
        }
        else if (strcmp(pairs[i].key, "post_ping_duration_us") == 0)
        {
            unsigned int duration = 0;
            AbortIfNot(sscanf(pairs[i].value, "%u", &duration), );

            params.post_ping_duration = micros_to_ticks(duration);
            dbprintf("Post-ping duration is %u us.\n", duration);
        }
        else if (strcmp(pairs[i].key, "reset") == 0)
        {
            /*
             * Trigger a software reset of the Zynq.
             */
            dbprintf("Resetting Zynq...");
            give_up();
        }
    }
}

/**
 * Issue a request for thruster silent running for clean hydrophone readings.
 *
 * @param socket The socket to make the request on.
 * @param ticks The time in the future that thrusters should be shut down at.
 * @param duration The duration that thrusters should be shut down for.
 *
 * @return Success or fail.
 */
result_t request_thruster_shutdown(udp_socket_t *socket,
                                   const tick_t future_ticks,
                                   const tick_t duration)
{
    char msg[8];
    int32_t when_ms = ticks_to_ms(future_ticks - get_system_time());
    int32_t duration_ms = ticks_to_ms(duration);
    memcpy(msg, &when_ms, 4);
    memcpy(&msg[4], &duration_ms, 4);

    AbortIfNot(send_udp(socket, msg, 8), fail);

    return success;
}

/**
 * Application process.
 *
 * @return Success or fail.
 */
result_t go()
{
    /*
     * Initialize the system.
     */
    AbortIfNot(init_system(), fail);

    dbprintf("Beginning HydroZynq main application\n");

    /*
     * Initialize the network stack with the specified IP address.
     */
    struct ip_addr our_ip, netmask, gateway;
    IP4_ADDR(&our_ip, 192, 168, 0, 7);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 192, 168, 1, 1);

    macaddr_t mac_address = {
        .addr = {0x00, 0x0a, 0x35, 0x00, 0x01, 0x02}
    };

    AbortIfNot(init_network_stack(our_ip, netmask, gateway, mac_address), fail);
    AbortIfNot(dbinit(), fail);
    dbprintf("Network stack initialized\n");

    /*
     * Initialize the DMA engine for reading samples.
     */
     AbortIfNot(initialize_dma(&dma, DMA_BASE_ADDRESS), fail);

    /*
     * Configure the ADC.
     */
    AbortIfNot(init_spi(&adc_spi, SPI_BASE_ADDRESS), fail);

    bool verify_write = false;
    bool use_test_pattern = false;
    AbortIfNot(init_adc(&adc, &adc_spi, ADC_BASE_ADDRESS, verify_write, use_test_pattern), fail);

    /*
     * Set the sample rate to 5MHz.
     */
    adc.regs->clk_div = 10;
    dbprintf("ADC clock div: %d\n", adc.regs->clk_div);
    dbprintf("ADC samples per packet: %d\n", adc.regs->samples_per_packet);

    /*
     * Bind the command port, data stream port, and the result output port.
     */
    udp_socket_t command_socket, data_stream_socket, result_socket, xcorr_stream_socket, silent_request_socket;

    struct ip_addr dest_ip;
    IP4_ADDR(&dest_ip, 192, 168, 0, 2);

    AbortIfNot(init_udp(&command_socket), fail);
    AbortIfNot(bind_udp(&command_socket, IP_ADDR_ANY, COMMAND_SOCKET_PORT, receive_command), fail);

    AbortIfNot(init_udp(&silent_request_socket), fail);
    AbortIfNot(connect_udp(&silent_request_socket, &dest_ip, SILENT_REQUEST_PORT), fail);

    AbortIfNot(init_udp(&data_stream_socket), fail);
    AbortIfNot(connect_udp(&data_stream_socket, &dest_ip, DATA_STREAM_PORT), fail);

    AbortIfNot(init_udp(&xcorr_stream_socket), fail);
    AbortIfNot(connect_udp(&xcorr_stream_socket, &dest_ip, XCORR_STREAM_PORT), fail);

    AbortIfNot(init_udp(&result_socket), fail);
    AbortIfNot(connect_udp(&result_socket, &dest_ip, RESULT_PORT), fail);

    dbprintf("System initialization complete. Start time: %d ms\n",
            ticks_to_ms(get_system_time()));

    set_interrupts(true);

    /*
     * Read the first sample from the ADC and ignore it - the ADC always has an
     * invalid measurement as the first reading.
     */
    AbortIfNot(record(&dma, samples, adc.regs->samples_per_packet, adc), fail);

    /*
     * Set up the initial parameters.
     */
    params.sample_clk_div = adc.regs->clk_div;
    params.samples_per_packet = adc.regs->samples_per_packet;
    params.ping_threshold = INITIAL_ADC_THRESHOLD;

    /*
     * Perform a correlation for two wavelengths after the threshold is
     * encountered. Before the threshold will be either the initial wavefronts
     * or white noise floor. Because the noise floor is small, it will not
     * affect the correlation.
     */
    params.pre_ping_duration = micros_to_ticks(100);
    params.post_ping_duration = micros_to_ticks(50);
    params.filter = false;

    tick_t previous_ping_tick = get_system_time();
    while (1)
    {
        const uint32_t sampling_frequency = FPGA_CLK / (params.sample_clk_div * 2);

        /*
         * Push received network traffic into the network stack.
         */
        dispatch_network_stack();

        /*
         * Find sync for the start of a ping if we are not debugging.
         */
        if (!sync && !debug_stream)
        {
            bool found = false;
            int sync_attempts = 0;
            analog_sample_t max_value;
            while (!found && !debug_stream)
            {
                uint32_t sample_duration_ms = 2100;
                uint32_t samples_to_take = sample_duration_ms / 1000.0 * sampling_frequency;
                AbortIfNot(acquire_sync(&dma,
                                        samples,
                                        samples_to_take,
                                        &previous_ping_tick,
                                        &found,
                                        &max_value,
                                        adc,
                                        sampling_frequency,
                                        &params,
                                        highpass_iir,
                                        5), fail);

                /*
                 * Dispatch the network stack during sync to ensure messages
                 * are properly transmitted.
                 */
                dispatch_network_stack();

                if (!found)
                {
                    dbprintf("Failed to find ping during sync phase: %d - MaxVal: %d\n", ++sync_attempts, max_value);
                }
            }

            if (found)
            {
                dbprintf("Synced: %f s - MaxVal: %d\n", ticks_to_seconds(previous_ping_tick), max_value);
                sync = true;
            }
        }

        /*
         * Fast forward the previous ping tick until the most likely time
         * of the most recent ping.
         */
        if (!debug_stream)
        {
            tick_t next_ping_tick = previous_ping_tick;
            while (get_system_time() > (next_ping_tick - ms_to_ticks(50)))
            {
                next_ping_tick += ms_to_ticks(2000);
            }

            /*
             * Request that thrusters enter shutdown at the next ping tick.
             */
            AbortIfNot(request_thruster_shutdown(
                        &silent_request_socket,
                        (next_ping_tick - ms_to_ticks(50)),
                        ms_to_ticks(100)), fail);

            /*
             * Wait until the ping is about to come (100ms before).
             */
            while (get_system_time() < (next_ping_tick - ms_to_ticks(50)));
        }

        /*
         * Record the ping.
         */
        uint32_t sample_duration_ms = (debug_stream)? 2100 : 300;
        uint32_t num_samples = sample_duration_ms / 1000.0 * sampling_frequency;
        if (num_samples % params.samples_per_packet)
        {
            num_samples += (params.samples_per_packet - (num_samples % params.samples_per_packet));
        }

        if (num_samples > MAX_SAMPLES)
        {
            num_samples = MAX_SAMPLES - (MAX_SAMPLES % adc.regs->samples_per_packet);
        }

        tick_t sample_start_tick = get_system_time();
        AbortIfNot(record(&dma, samples, num_samples, adc), fail);

        AbortIfNot(normalize(samples, num_samples), fail);

        /*
         * Filter the received signal.
         */
        if (params.filter)
        {
            const tick_t filter_start_time = get_system_time();
            AbortIfNot(filter(samples, num_samples, highpass_iir, 5), fail);

            dbprintf("Filtering took %lf seconds.\n",
                    ticks_to_seconds(get_system_time() - filter_start_time));
        }

        /*
         * If debugging is enabled, don't perform the correlation or truncation
         * steps and just dump data.
         */
        if (debug_stream)
        {
            AbortIfNot(send_data(&data_stream_socket, samples, num_samples), fail);
            continue;
        }

        /*
         * Truncate the data around the ping.
         */
        size_t start_index, end_index;
        bool located = false;
        AbortIfNot(truncate(samples, num_samples, &start_index, &end_index, &located, params, sampling_frequency), fail);

        sync = located;
        if (!sync)
        {
            dbprintf("Failed to find the ping.\n");
            continue;
        }

        tick_t offset = start_index * (CPU_CLOCK_HZ / sampling_frequency);
        previous_ping_tick = sample_start_tick + offset;
        dbprintf("Found ping: %f s\n", ticks_to_seconds(previous_ping_tick));

        /*
         * Locate the ping samples.
         */
        AbortIfNot(end_index > start_index, fail);
        sample_t *ping_start = &samples[start_index];
        size_t ping_length = end_index - start_index;

        /*
         * Perform the correlation on the data.
         */
        correlation_result_t result;
        size_t num_correlations;

        tick_t start_time = get_system_time();
        AbortIfNot(cross_correlate(ping_start, ping_length, correlations, MAX_SAMPLES * 2, &num_correlations, &result, sampling_frequency), fail);

        tick_t duration_time = get_system_time() - start_time;
        dbprintf("Correlation took %d ms\n", ticks_to_ms(duration_time));
        dbprintf("Correlation results: %d %d %d\n", result.channel_delay_ns[0], result.channel_delay_ns[1], result.channel_delay_ns[2]);

        /*
         * Relay the result.
         */
        AbortIfNot(send_result(&result_socket, &result), fail);

        /*
         * Send the data for the correlation portion and the correlation result.
         */
        AbortIfNot(send_xcorr(&xcorr_stream_socket, correlations, num_correlations), fail);
        AbortIfNot(send_data(&data_stream_socket, ping_start, ping_length), fail);
    }
}

/**
 * Main entry point into the application.
 *
 * @return Zero upon success.
 */
int main()
{
    go();

    /*
     * If go fails, we need to trigger a processor reset.
     */
    while (1)
    {
        give_up();
    }
}
