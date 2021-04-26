# cupti-tracer

#### Install dependencies
```
sudo yum install perf
sudo apt-get install python3-dev
sudo apt-get install libunwind-dev
```
#### Compile
```
nvcc -c --ptxas-options=-v --compiler-options '-fPIC'  -I./include/ -I../../../../include -I../../include -I/usr/include/python3.6/ smprofiler.cpp
nvcc -c --ptxas-options=-v --compiler-options '-fPIC'  -I./include/ -I../../../../include -I../../include -I/usr/include/python3.6/ smprofiler_timeline.cpp
nvcc -c --ptxas-options=-v --compiler-options '-fPIC'  -I./include/ -I../../../../include -I../../include -I/usr/include/python3.6/ perf_collector.cpp
nvcc -shared smprofiler.o smprofiler_timeline.o perf_collector.o -L /usr/lib/x86_64-linux-gnu/ -lunwind -L ../../lib64  -lcuda -L ../../../../lib64 -lcupti -I../../../../include -I../../include -I/usr/include/python3.6/ -o smprofiler.so
```

#### Run the tool

To run the tool, just import the python module into your training script and start the profiler.
```
import smprofiler

for epoch in range(1):  
      for i, data in enumerate(trainloader, 0):
         
          inputs, labels = data
          smprofiler.start('tensor copy')
          inputs = inputs.to('cuda')
          labels = labels.to('cuda')
          smprofiler.stop()
          
         
          optimizer.zero_grad()
          
          smprofiler.start("forward")
          outputs = net(inputs)
          smprofiler.stop()

          smprofiler.start("loss")
          loss = criterion(outputs, labels)
          smprofiler.stop()

          smprofiler.start("backward")
          loss.backward()
          smprofiler.stop()

          smprofiler.start("otpimizer")
          optimizer.step()
          smprofiler.stop()

```
For an example check out ![train.py](https://github.com/NRauschmayr/cupti-tracer/blob/main/train.py)
