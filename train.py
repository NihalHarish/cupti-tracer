import torch
import torchvision
import torchvision.transforms as transforms
import argparse
from torchvision import datasets, models, transforms
import smprofiler

transform = transforms.Compose(
    [transforms.ToTensor(),
     transforms.Resize(256),   
     transforms.Normalize((0.5, 0.5, 0.5), (0.5, 0.5, 0.5))])

trainset = torchvision.datasets.CIFAR10(root='./data', train=True,
                                        download=True, transform=transform)
trainloader = torch.utils.data.DataLoader(trainset, batch_size=64,
                                          shuffle=True, num_workers=2)

net = models.resnet101(pretrained=True)
net = net.to('cuda')

criterion = torch.nn.CrossEntropyLoss()
optimizer = torch.optim.SGD(net.parameters(), lr=0.001, momentum=0.9)

for epoch in range(3):  # loop over the dataset multiple times
    for i, data in enumerate(trainloader, 0):
        # get the inputs; data is a list of [inputs, labels]
        inputs, labels = data
        inputs = inputs.to('cuda')
        labels = labels.to('cuda')
        
        # zero the parameter gradients
        optimizer.zero_grad()

        # annotate with smprofiler
        smprofiler.start("forward")
        outputs = net(inputs)
        smprofiler.stop()
            
        loss = criterion(outputs, labels)
            
        smprofiler.start("backward")
        loss.backward()
        smprofiler.stop()

        optimizer.step()


