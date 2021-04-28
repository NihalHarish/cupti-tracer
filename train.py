import torch
import torchvision
import torchvision.transforms as transforms
import argparse
from torchvision import datasets, models, transforms
import smprofiler

transform = transforms.Compose(
    [
     transforms.Resize(256),
     transforms.ToTensor(),
     transforms.Normalize((0.5, 0.5, 0.5), (0.5, 0.5, 0.5))])

trainset = torchvision.datasets.CIFAR10(root='./data', train=True,
                                        download=True, transform=transform)
trainloader = torch.utils.data.DataLoader(trainset, batch_size=64,
                                          shuffle=True, num_workers=2)

net = models.resnet101(pretrained=True)
net = net.to('cuda')

criterion = torch.nn.CrossEntropyLoss()
optimizer = torch.optim.SGD(net.parameters(), lr=0.001, momentum=0.9)

for epoch in range(1):
    for i, data in enumerate(trainloader, 0):

        inputs, labels = data
        smprofiler.start("tensor_copy")
        inputs = inputs.to('cuda')
        labels = labels.to('cuda')
        smprofiler.stop()

        # zero the parameter gradients
        optimizer.zero_grad()

        # annotate with smprofiler
        smprofiler.start("forward")
        outputs = net(inputs)
        smprofiler.stop()

        smprofiler.start("loss")
        loss = criterion(outputs, labels)
        smprofiler.stop()

        smprofiler.start("backward")
        loss.backward()
        smprofiler.stop()

        smprofiler.start("optimizer")
        optimizer.step()
        smprofiler.stop()

        if i == 3:
            break
