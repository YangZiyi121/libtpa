#server side: sudo TPA_ID=server TPA_ETH_DEV=enp195s0f1np1 tpa run build/bin/app/tperf -s -n 1 -S 1 -p 3000
#client side: sudo -E TPA_ID=client TPA_ETH_DEV=enp194s0f1np1 TPA_CFG="tcp {tso = 0; }"   tpa run build/bin/app/tperf -c 172.24.5.50 -n 1 -t rr -Z 0 -S 0 -p 3000 -m 1024   -E /home/yangz0e/trace_libtpa/libtpa/processed_trace.csv


import csv

def preprocess_trace(csv_file, output_file):
    results = []

    with open(csv_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            duration = float(row['duration'])               # Sleep time
            fragments = int(row['fragments'])
            request_size = fragments * 256                  # Request size in bytes

            # map original func -> app: 1->1, 2->3, 3->5, 4->2
            func = int(row['func'])
            func_to_app = {1: 1, 2: 3, 3: 5, 4: 2}
            app = func_to_app.get(func, func)

            # response size rules: app 1,2 => 64; app 3,5 => req_size - 64
            if app in (1, 2):
                response_size = 64
            elif app in (3, 5):
                response_size = max(request_size, 0)
            else:
                # default: keep same as request size (fallback)
                response_size = request_size - 64

            results.append({
                'app': app,
                'sleep_time': duration,
                'request_size': request_size,
                'response_size': response_size
            })

    # Write processed result
    with open(output_file, 'w') as f:
        writer = csv.DictWriter(f, fieldnames=['app', 'sleep_time', 'request_size', 'response_size'])
        writer.writeheader()
        writer.writerows(results)

    print(f"Processed {len(results)} entries → saved to {output_file}")


# Example usage
preprocess_trace('mixed_1h.csv', 'processed_trace.csv')

